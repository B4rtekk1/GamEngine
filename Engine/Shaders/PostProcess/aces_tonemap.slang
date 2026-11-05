struct TonemapVertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
};

[shader("vertex")]
TonemapVertexOutput vertexMain(uint vertexIndex : SV_VertexID)
{
    TonemapVertexOutput output;
    float2 position = float2((vertexIndex << 1) & 2, vertexIndex & 2);
    output.uv = position;
    output.position = float4(position * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

[[vk::binding(0, 0)]]
Sampler2D hdrImage;
[[vk::binding(1, 0)]]
Sampler2D bloomImage;
// Screen motion in UV, including the TAA jitter delta (under one pixel).
[[vk::binding(2, 0)]]
Sampler2D velocityImage;

struct TonemapSettings
{
    float exposure;
    float applyGamma;
    // Contrast-adaptive sharpening strength in [0, 1] for moving pixels;
    // zero disables it.
    float sharpness;
};

[[vk::push_constant]]
ConstantBuffer<TonemapSettings> settings;

struct TonemapFragmentInput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
};

struct TonemapFragmentOutput
{
    [[vk::location(0)]] float4 color : SV_Target;
};

float3 acesFilm(float3 color)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) /
                 (color * (c * color + d) + e), 0.0, 1.0);
}

// Applying a curve independently to R, G and B drives very bright coloured
// emitters toward white.  Compress scene luminance instead, then rescale the
// original linear colour; bloom can stay HDR while neon retains its hue.
float3 acesFilmLuminancePreserving(float3 color)
{
    const float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    if (luminance <= 1.0e-5)
        return 0.0;

    const float mappedLuminance = acesFilm(float3(luminance)).x;
    return color * (mappedLuminance / luminance);
}

float3 compressHdr(float3 color) { return color / (1.0 + color); }
float3 expandHdr(float3 color) { return color / max(1.0 - color, 1.0e-4); }

// AMD CAS-style sharpening on the cross neighbourhood. The weight falls off
// where the local range is already near black or white, so edges do not
// ring and flat areas are left untouched. Runs in a Reinhard-compressed
// space so bright HDR texels cannot dominate it.
float3 sharpenedHdr(int2 pixel, float sharpness)
{
    uint width, height;
    hdrImage.GetDimensions(width, height);
    const int2 lastPixel = int2(width, height) - 1;
    const float3 c = compressHdr(max(hdrImage.Load(int3(pixel, 0)).rgb, 0.0));
    const float3 n = compressHdr(max(hdrImage.Load(int3(clamp(pixel + int2(0, -1), 0, lastPixel), 0)).rgb, 0.0));
    const float3 s = compressHdr(max(hdrImage.Load(int3(clamp(pixel + int2(0, 1), 0, lastPixel), 0)).rgb, 0.0));
    const float3 w = compressHdr(max(hdrImage.Load(int3(clamp(pixel + int2(-1, 0), 0, lastPixel), 0)).rgb, 0.0));
    const float3 e = compressHdr(max(hdrImage.Load(int3(clamp(pixel + int2(1, 0), 0, lastPixel), 0)).rgb, 0.0));
    const float3 minimum = min(c, min(min(n, s), min(w, e)));
    const float3 maximum = max(c, max(max(n, s), max(w, e)));
    const float3 amplitude = sqrt(saturate(min(minimum, 1.0 - maximum) / max(maximum, 1.0e-5)));
    const float3 weight = -amplitude * lerp(0.125, 0.2, sharpness);
    const float3 sharpened = (c + (n + s + w + e) * weight) / (1.0 + 4.0 * weight);
    return expandHdr(saturate(sharpened));
}

float interleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

[shader("fragment")]
TonemapFragmentOutput fragmentMain(TonemapFragmentInput input)
{
    TonemapFragmentOutput output;
    // Ramp in between one and two pixels of motion: the jitter delta alone
    // stays below one pixel, so a static image is never sharpened.
    float motionSharpness = 0.0;
    if (settings.sharpness > 0.0)
    {
        uint width, height;
        velocityImage.GetDimensions(width, height);
        const float motionPixels = length(velocityImage.Load(int3(int2(input.position.xy), 0)).xy *
                                          float2(width, height));
        motionSharpness = settings.sharpness * saturate(motionPixels - 1.0);
    }
    const float3 scene = motionSharpness > 0.0 ? sharpenedHdr(int2(input.position.xy), motionSharpness)
                                               : hdrImage.Sample(input.uv).rgb;
    float3 color = (scene + bloomImage.Sample(input.uv).rgb * 0.12) * exp2(settings.exposure);
    color = acesFilmLuminancePreserving(color);
    // The display target is normalized; clipping now happens only after hue
    // preservation rather than as a side effect of per-channel tone mapping.
    color = saturate(color);
    if (settings.applyGamma > 0.5)
        color = pow(color, float3(1.0 / 2.2));
    // One display-space LSB decorrelates smooth post-process gradients from
    // 8-bit swapchain quantization without producing visible grain.
    color = saturate(color + (interleavedGradientNoise(input.position.xy) - 0.5) / 255.0);
    output.color = float4(color, 1.0);
    return output;
}
