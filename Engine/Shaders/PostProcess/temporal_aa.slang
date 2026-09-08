struct VertexOutput { float4 position : SV_Position; [[vk::location(0)]] float2 uv; };
[shader("vertex")]
VertexOutput vertexMain(uint index : SV_VertexID) {
    VertexOutput output; float2 p = float2((index << 1) & 2, index & 2);
    output.uv = p; output.position = float4(p * 2.0 - 1.0, 0.0, 1.0); return output;
}

[[vk::binding(0, 0)]] Sampler2D currentFrame;
[[vk::binding(1, 0)]] Sampler2D historyFrame;
[[vk::binding(2, 0)]] Sampler2D velocityFrame;
struct Settings {
    float currentJitterX, currentJitterY, previousJitterX, previousJitterY;
    float historyWeight, inverseWidth, inverseHeight, padding;
};
[[vk::push_constant]] ConstantBuffer<Settings> settings;
struct FragmentInput { [[vk::location(0)]] float2 uv; };
struct FragmentOutput { [[vk::location(0)]] float4 color : SV_Target; };

float3 reinhard(float3 color) { return color / (color + 1.0); }
float3 reinhardInverse(float3 color) { return color / max(1.0 - color, 1e-5); }
float luminance(float3 color) { return max(dot(color, float3(0.299, 0.587, 0.114)), 1e-4); }

// Nine-tap Catmull-Rom history reconstruction, as used by Godot's TAA resolve.
float3 sampleHistoryCatmullRom(float2 uv, float2 resolution) {
    float2 samplePos = uv * resolution;
    float2 texPos1 = floor(samplePos - 0.5) + 0.5;
    float2 f = samplePos - texPos1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 texPos0 = (texPos1 - 1.0) / resolution;
    float2 texPos3 = (texPos1 + 2.0) / resolution;
    float2 texPos12 = (texPos1 + w2 / max(w12, 1e-5)) / resolution;
    float3 result = 0.0;
    result += historyFrame.Sample(float2(texPos0.x, texPos0.y)).rgb * w0.x * w0.y;
    result += historyFrame.Sample(float2(texPos12.x, texPos0.y)).rgb * w12.x * w0.y;
    result += historyFrame.Sample(float2(texPos3.x, texPos0.y)).rgb * w3.x * w0.y;
    result += historyFrame.Sample(float2(texPos0.x, texPos12.y)).rgb * w0.x * w12.y;
    result += historyFrame.Sample(float2(texPos12.x, texPos12.y)).rgb * w12.x * w12.y;
    result += historyFrame.Sample(float2(texPos3.x, texPos12.y)).rgb * w3.x * w12.y;
    result += historyFrame.Sample(float2(texPos0.x, texPos3.y)).rgb * w0.x * w3.y;
    result += historyFrame.Sample(float2(texPos12.x, texPos3.y)).rgb * w12.x * w3.y;
    result += historyFrame.Sample(float2(texPos3.x, texPos3.y)).rgb * w3.x * w3.y;
    return max(result, 0.0);
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
    const float4 velocitySample = velocityFrame.Sample(input.uv);

    // Our velocity is current UV - previous UV, unlike Godot's convention.
    // Velocity-less sky/particles instead use the projection-jitter delta.
    const float2 jitterHistoryUv = input.uv - 0.5 * float2(
        settings.currentJitterX - settings.previousJitterX,
        settings.currentJitterY - settings.previousJitterY);
    const float2 historyUv = velocitySample.a >= 0.5
        ? input.uv - velocitySample.xy : jitterHistoryUv;
    const bool historyInBounds = all(historyUv >= 0.0) && all(historyUv <= 1.0);

    float3 mean = 0.0;
    float3 meanSquared = 0.0;
    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
        const float3 sample = currentFrame.Sample(input.uv + float2(x, y) * texel).rgb;
        mean += sample; meanSquared += sample * sample;
    }
    mean *= 1.0 / 9.0; meanSquared *= 1.0 / 9.0;
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
    const float currentWeight = settings.historyWeight > 0.0
        ? (1.0 - settings.historyWeight) * similarity * similarity : 1.0;
    const float3 resolved = reinhardInverse(lerp(historyTm, currentTm,
        historyInBounds ? currentWeight : 1.0));
    output.color = float4(resolved, 1.0);
    return output;
}
