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

struct TonemapSettings
{
    float exposure;
    float applyGamma;
};

[[vk::push_constant]]
ConstantBuffer<TonemapSettings> settings;

struct TonemapFragmentInput
{
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

[shader("fragment")]
TonemapFragmentOutput fragmentMain(TonemapFragmentInput input)
{
    TonemapFragmentOutput output;
    float3 color = (hdrImage.Sample(input.uv).rgb + bloomImage.Sample(input.uv).rgb * 0.12) * exp2(settings.exposure);
    color = acesFilmLuminancePreserving(color);
    // The display target is normalized; clipping now happens only after hue
    // preservation rather than as a side effect of per-channel tone mapping.
    color = saturate(color);
    if (settings.applyGamma > 0.5)
        color = pow(color, float3(1.0 / 2.2));
    output.color = float4(color, 1.0);
    return output;
}
