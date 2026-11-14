// Final camera and display transform: lens effects on the scene-referred HDR
// image (motion blur, depth of field, chromatic aberration), exposure, white
// balance and grading, the AgX or ACES display curve, vignette and dither.
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
// Linear view depth of the displayed frame (the TAA resolve writes it).
[[vk::binding(3, 0)]]
Sampler2D depthImage;
// 1x1 adapted scene luminance from auto_exposure.
[[vk::binding(4, 0)]]
Sampler2D exposureImage;

struct TonemapSettings
{
    // Exposure compensation in stops, added to the metered exposure.
    float exposure;
    float applyGamma;
    // Contrast-adaptive sharpening strength in [0, 1] for moving pixels;
    // zero disables it.
    float sharpness;
    // 1 when exposureImage holds a metered luminance.
    float autoExposure;
    // 0: AgX, 1: luminance-preserving ACES.
    float tonemapper;
    float contrast;
    float saturation;
    float temperature;
    float tint;
    float vignette;
    float chromaticAberration;
    // Shutter fraction; 0 disables motion blur (also without velocity).
    float motionBlur;
    // 1 when depth of field is enabled and depthImage is valid.
    float depthOfField;
    float focusDistance;
    float aperture;
    float maxBlurPixels;
    // Weight of the summed bloom mip chain (BloomPass).
    float bloom;
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

static const float MiddleGrey = 0.18;
static const float3 LuminanceWeights = float3(0.2126, 0.7152, 0.0722);

float luminance(float3 color)
{
    return dot(color, LuminanceWeights);
}

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
    const float sceneLuminance = luminance(color);
    if (sceneLuminance <= 1.0e-5)
        return 0.0;

    const float mappedLuminance = acesFilm(float3(sceneLuminance)).x;
    return color * (mappedLuminance / sceneLuminance);
}

// AgX (Troy Sobotka), in Benjamin Wrensch's polynomial fit. The inset
// matrix mixes a little of every channel into the others before the log
// curve, so saturated light that overexposes desaturates smoothly to white
// instead of clipping to a flat, garish primary. Returns linear sRGB.
float3 agxContrast(float3 x)
{
    const float3 x2 = x * x;
    const float3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

// With `displayEncoded` the result stays in AgX's display encoding instead:
// the closing pow(2.2) and a following manual pow(1 / 2.2) gamma cancel.
float3 agx(float3 color, bool displayEncoded)
{
    const float3x3 inset = float3x3(
        0.842479062253094, 0.0423282422610123, 0.0423756549057051,
        0.0784335999999992, 0.878468636469772, 0.0784336,
        0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float3x3 outset = float3x3(
        1.19687900512017, -0.0528968517574562, -0.0529716355144438,
        -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
        -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393;
    const float maxEv = 4.026069;
    float3 encoded = mul(max(color, 0.0), inset);
    encoded = clamp(log2(max(encoded, 1.0e-10)), minEv, maxEv);
    encoded = (encoded - minEv) / (maxEv - minEv);
    encoded = agxContrast(encoded);
    // Base AgX is deliberately flat; on its own it left sunlit and shadowed
    // surfaces too close together. This look adds contrast around middle grey
    // (0.18 encodes to ~0.5, so the metered exposure is unchanged) and
    // restores some of the saturation the inset removed, like Blender's
    // "Punchy" look without its overall darkening.
    const float lookPivot = 0.5;
    const float lookContrast = 1.35;
    const float lookSaturation = 1.25;
    encoded = lookPivot * pow(max(encoded, 0.0) / lookPivot, lookContrast);
    const float encodedLuma = dot(encoded, LuminanceWeights);
    encoded = encodedLuma + lookSaturation * (encoded - encodedLuma);
    const float3 display = saturate(mul(encoded, outset));
    return displayEncoded ? display : pow(display, 2.2);
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
    // The negative weights extrapolate past the cross neighbourhood: a bright
    // texel among dark ones would saturate to 1.0, which expandHdr maps to a
    // 1e4 white dot. Staying inside the local range removes the overshoot.
    return expandHdr(clamp(sharpened, minimum, maximum));
}

float interleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// Circle of confusion in pixels. A thin lens blurs in proportion to
// |1/focus - 1/depth|; scaling by the focus distance makes `aperture` the
// blur of the far background as a fraction of maxBlurPixels.
float circleOfConfusion(float depth)
{
    const float focus = max(settings.focusDistance, 0.05);
    const float defocus = abs(1.0 - focus / max(depth, 1.0e-3));
    return min(defocus * settings.aperture, 1.0) * settings.maxBlurPixels;
}

// Gather depth of field on a 24-tap Vogel disc. A tap contributes where its
// own blur disc reaches this pixel; a tap behind the receiver is limited to
// the receiver's blur, so an out-of-focus background never smears over a
// sharp foreground silhouette.
float3 depthOfField(float2 uv, float2 texel, float noise)
{
    const float centreDepth = depthImage.SampleLevel(uv, 0.0).r;
    const float centreCoc = circleOfConfusion(centreDepth);
    const float3 centre = hdrImage.SampleLevel(uv, 0.0).rgb;
    // Foreground taps can blur over a sharp pixel, so search the full radius.
    const float searchRadius = settings.maxBlurPixels * min(settings.aperture, 1.0);
    if (searchRadius < 0.5)
        return centre;
    float3 sum = centre;
    float weightSum = 1.0;
    const uint TapCount = 24u;
    const float goldenAngle = 2.39996323;
    [loop] for (uint tap = 0u; tap < TapCount; ++tap)
    {
        const float radius = sqrt((float(tap) + 0.5) / float(TapCount)) * max(centreCoc, searchRadius * 0.5);
        const float angle = (float(tap) + noise) * goldenAngle;
        const float2 offset = float2(cos(angle), sin(angle)) * radius;
        const float2 tapUv = uv + offset * texel;
        const float tapDepth = depthImage.SampleLevel(tapUv, 0.0).r;
        float tapCoc = circleOfConfusion(tapDepth);
        if (tapDepth > centreDepth)
            tapCoc = min(tapCoc, centreCoc);
        const float weight = saturate(tapCoc - radius + 1.0);
        sum += hdrImage.SampleLevel(tapUv, 0.0).rgb * weight;
        weightSum += weight;
    }
    return sum / weightSum;
}

// Blur along this pixel's motion over the open-shutter interval. Taps are
// centred on the pixel, so a moving object smears both ways like a real
// exposure and the image does not shift.
float3 motionBlur(float2 uv, float2 motionUv, float noise)
{
    const uint TapCount = 10u;
    float3 sum = 0.0;
    [loop] for (uint tap = 0u; tap < TapCount; ++tap)
    {
        const float t = (float(tap) + noise) / float(TapCount) - 0.5;
        sum += hdrImage.SampleLevel(uv + motionUv * t, 0.0).rgb;
    }
    return sum / float(TapCount);
}

// Radial colour split towards the frame edge, as a cheap lens would.
float3 chromaticAberration(float2 uv, float3 centre)
{
    const float2 fromCentre = uv - 0.5;
    const float2 shift = fromCentre * dot(fromCentre, fromCentre) * settings.chromaticAberration * 0.05;
    return float3(hdrImage.SampleLevel(uv + shift, 0.0).r, centre.g, hdrImage.SampleLevel(uv - shift, 0.0).b);
}

// White balance as a von Kries-style channel scale normalised to keep
// luminance: temperature trades blue for red, tint trades magenta for green.
float3 whiteBalance(float3 color)
{
    const float3 scale = float3(1.0 + 0.2 * settings.temperature - 0.05 * settings.tint,
                                1.0 + 0.15 * settings.tint,
                                1.0 - 0.2 * settings.temperature - 0.05 * settings.tint);
    return color * (scale / max(luminance(scale), 1.0e-4));
}

float3 grade(float3 color)
{
    color = whiteBalance(color);
    // Contrast around middle grey in log space, as a film grade would; it
    // leaves the exposed middle grey exactly where the meter put it.
    // pow is two quarter-rate instructions per channel; skip the identity.
    if (settings.contrast != 1.0)
        color = MiddleGrey * pow(max(color, 0.0) / MiddleGrey, settings.contrast);
    const float grey = luminance(color);
    return max(lerp(float3(grey), color, settings.saturation), 0.0);
}

[shader("fragment")]
TonemapFragmentOutput fragmentMain(TonemapFragmentInput input)
{
    TonemapFragmentOutput output;
    uint width, height;
    hdrImage.GetDimensions(width, height);
    const float2 resolution = float2(width, height);
    const float2 texel = 1.0 / resolution;
    const int2 pixel = int2(input.position.xy);
    const float noise = interleavedGradientNoise(input.position.xy);

    float2 motionUv = 0.0;
    float motionPixels = 0.0;
    if (settings.sharpness > 0.0 || settings.motionBlur > 0.0)
    {
        uint velocityWidth, velocityHeight;
        velocityImage.GetDimensions(velocityWidth, velocityHeight);
        motionUv = velocityImage.Load(int3(pixel * int2(velocityWidth, velocityHeight) / int2(width, height), 0)).xy;
        motionPixels = length(motionUv * resolution);
    }

    float3 scene;
    // Motion below two pixels is mostly TAA jitter and is left sharp.
    const float blurPixels = motionPixels * settings.motionBlur;
    if (settings.motionBlur > 0.0 && blurPixels > 2.0)
    {
        // Cap the streak so a camera cut cannot smear the whole frame.
        const float maxStreakPixels = 48.0;
        scene = motionBlur(input.uv, motionUv * settings.motionBlur * min(1.0, maxStreakPixels / blurPixels), noise);
    }
    else if (settings.depthOfField > 0.5)
    {
        scene = depthOfField(input.uv, texel, noise);
    }
    else
    {
        // Ramp in between one and two pixels of motion: the jitter delta
        // alone stays below one pixel, so a static image is never sharpened.
        const float motionSharpness = settings.sharpness * saturate(motionPixels - 1.0);
        scene = motionSharpness > 0.0 ? sharpenedHdr(pixel, motionSharpness) : hdrImage.SampleLevel(input.uv, 0.0).rgb;
        if (settings.chromaticAberration > 0.0)
            scene = chromaticAberration(input.uv, scene);
    }

    float exposure = exp2(settings.exposure);
    if (settings.autoExposure > 0.5)
        exposure *= MiddleGrey / max(exposureImage.Load(int3(0, 0, 0)).r, 1.0e-6);
    float3 color = (scene + bloomImage.SampleLevel(input.uv, 0.0).rgb * settings.bloom) * exposure;
    color = grade(color);
    const bool manualGamma = settings.applyGamma > 0.5;
    const bool agxEncoded = manualGamma && settings.tonemapper < 0.5;
    color = settings.tonemapper < 0.5 ? agx(color, agxEncoded) : acesFilmLuminancePreserving(color);
    // The display target is normalized; clipping now happens only after the
    // display curve rather than as a side effect of it.
    color = saturate(color);

    // Natural (cos^4-like) falloff, applied in display-linear light.
    if (settings.vignette > 0.0)
    {
        const float2 fromCentre = (input.uv - 0.5) * float2(resolution.x / resolution.y, 1.0);
        const float falloff = saturate(1.0 - dot(fromCentre, fromCentre) * settings.vignette);
        // Display-encoded AgX takes the gain through the same gamma.
        color *= agxEncoded ? pow(falloff, 2.0 / 2.2) : falloff * falloff;
    }

    if (manualGamma && !agxEncoded)
        color = pow(color, float3(1.0 / 2.2));
    // One display-space LSB decorrelates smooth post-process gradients from
    // 8-bit swapchain quantization without producing visible grain.
    color = saturate(color + (noise - 0.5) / 255.0);
    output.color = float4(color, 1.0);
    return output;
}
