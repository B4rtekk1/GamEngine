struct VertexOutput { float4 position : SV_Position; [[vk::location(0)]] float2 uv; };
[shader("vertex")]
VertexOutput vertexMain(uint index : SV_VertexID) {
    VertexOutput output; float2 p = float2((index << 1) & 2, index & 2);
    output.uv = p; output.position = float4(p * 2.0 - 1.0, 0.0, 1.0); return output;
}

[[vk::binding(0, 0)]] Sampler2D currentFrame;
[[vk::binding(1, 0)]] Sampler2D historyFrame;
[[vk::binding(2, 0)]] Sampler2D velocityFrame;
[[vk::binding(3, 0)]] Sampler2D currentDepthFrame;
[[vk::binding(4, 0)]] Sampler2D historyDepthFrame;
struct Settings {
    float currentJitterX, currentJitterY, previousJitterX, previousJitterY;
    float historyWeight, inverseWidth, inverseHeight, padding;
};
[[vk::push_constant]] ConstantBuffer<Settings> settings;
struct FragmentInput { [[vk::location(0)]] float2 uv; };
struct FragmentOutput {
    [[vk::location(0)]] float4 color : SV_Target;
    [[vk::location(1)]] float4 depth : SV_Target1;
};

float3 reinhard(float3 color) { return color / (color + 1.0); }
float3 reinhardInverse(float3 color) { return color / max(1.0 - color, 1e-5); }
float luminance(float3 color) { return max(dot(color, float3(0.299, 0.587, 0.114)), 1e-4); }

// Five-tap Catmull-Rom-style history reconstruction. Four bilinear samples
// coalesce the 4x4 cubic footprint; a small centre contribution suppresses
// ringing from the negative Catmull-Rom lobes.
float3 sampleHistoryCatmullRom(float2 uv, float2 resolution) {
    float2 samplePos = uv * resolution;
    float2 texPos1 = floor(samplePos - 0.5) + 0.5;
    float2 f = samplePos - texPos1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w01 = w0 + w1;
    const float2 w23 = w2 + w3;
    const float2 uv01 = (texPos1 - 1.0 + w1 / max(w01, 1e-5)) / resolution;
    const float2 uv23 = (texPos1 + 1.0 + w3 / max(w23, 1e-5)) / resolution;

    float3 cubic = 0.0;
    cubic += historyFrame.Sample(float2(uv01.x, uv01.y)).rgb * w01.x * w01.y;
    cubic += historyFrame.Sample(float2(uv23.x, uv01.y)).rgb * w23.x * w01.y;
    cubic += historyFrame.Sample(float2(uv01.x, uv23.y)).rgb * w01.x * w23.y;
    cubic += historyFrame.Sample(float2(uv23.x, uv23.y)).rgb * w23.x * w23.y;

    const float3 centre = historyFrame.Sample(uv).rgb;
    return max(lerp(cubic, centre, 0.125), 0.0);
}

// Playdead/Godot AABB clipping avoids the colour shifts of per-channel clamp.
float3 clipAabb(float3 boxMin, float3 boxMax, float3 center, float3 history) {
    float3 direction = history - center;
    float3 toMax = boxMax - center;
    float3 toMin = boxMin - center;
    if (direction.x > toMax.x + 1e-5) direction *= toMax.x / direction.x;
    if (direction.y > toMax.y + 1e-5) direction *= toMax.y / direction.y;
    if (direction.z > toMax.z + 1e-5) direction *= toMax.z / direction.z;
    if (direction.x < toMin.x - 1e-5) direction *= toMin.x / direction.x;
    if (direction.y < toMin.y - 1e-5) direction *= toMin.y / direction.y;
    if (direction.z < toMin.z - 1e-5) direction *= toMin.z / direction.z;
    return center + direction;
}

[shader("fragment")]
FragmentOutput fragmentMain(FragmentInput input) {
    FragmentOutput output;
    const float2 texel = float2(settings.inverseWidth, settings.inverseHeight);
    const float2 resolution = 1.0 / texel;
    const float3 current = currentFrame.Sample(input.uv).rgb;
    const float currentDepth = currentDepthFrame.Sample(input.uv).r;
    // At a silhouette, bilinear velocity may contain the background's motion
    // even though the current pixel belongs to the foreground.  Vulkan's
    // depth convention is near=0, so select the velocity at the nearest depth
    // in a compact 3x3 footprint before reprojection.
    float nearestDepth = currentDepth;
    float2 velocitySample = velocityFrame.Sample(input.uv).xy;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            const float2 sampleUv = clamp(input.uv + float2(x, y) * texel, 0.0, 1.0);
            const float sampleDepth = currentDepthFrame.Sample(sampleUv).r;
            if (sampleDepth < nearestDepth) {
                nearestDepth = sampleDepth;
                velocitySample = velocityFrame.Sample(sampleUv).xy;
            }
        }
    }

    // Our velocity is current UV - previous UV, unlike Godot's convention.
    const float2 historyUv = input.uv - velocitySample;
    const bool historyInBounds = all(historyUv >= 0.0) && all(historyUv <= 1.0);
    const float historyDepth = historyDepthFrame.Sample(historyUv).r;
    // The stored device depth is deliberately sampled nearest.  This is a
    // conservative visibility test: rejecting a valid history costs a little
    // stability, whereas accepting a different surface creates a trail.
    const float depthTolerance = max(0.0025, max(currentDepth, historyDepth) * 0.02);
    const bool depthMatches = abs(currentDepth - historyDepth) <= depthTolerance;

    // Full 3x3 neighbourhood captures diagonal and one-pixel geometry that
    // a cross-shaped kernel misses.  Its variance bounds feed AABB clipping.
    float3 mean = 0.0;
    float3 meanSquared = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            const float3 sample = currentFrame.Sample(clamp(input.uv + float2(x, y) * texel, 0.0, 1.0)).rgb;
            mean += sample;
            meanSquared += sample * sample;
        }
    }
    mean *= (1.0 / 9.0);
    meanSquared *= (1.0 / 9.0);
    const float varianceScale = clamp(1.1 * 1080.0 * settings.inverseHeight, 0.75, 1.0);
    const float3 deviation = sqrt(abs(meanSquared - mean * mean)) * varianceScale;
    float3 history = sampleHistoryCatmullRom(historyUv, resolution);
    history = clipAabb(mean - deviation, mean + deviation, mean, history);

    // Resolve HDR in a bounded domain, following Godot's flicker suppression.
    const float3 currentTm = reinhard(current);
    const float3 historyTm = reinhard(history);
    const float luminanceDifference = abs(luminance(currentTm) - luminance(historyTm)) /
        max(luminance(currentTm), max(luminance(historyTm), 1.001));
    const float similarity = 1.0 - luminanceDifference;
    // Retain history only when its luminance agrees with this frame. A large
    // difference is a disocclusion/change signal, so it must favour current.
    const float historyConfidence = similarity * similarity;
    const float historyWeight = settings.historyWeight * historyConfidence;
    const float3 resolved = reinhardInverse(lerp(currentTm, historyTm,
        (historyInBounds && depthMatches) ? historyWeight : 0.0));
    output.color = float4(resolved, 1.0);
    output.depth = float4(currentDepth, 0.0, 0.0, 1.0);
    return output;
}
