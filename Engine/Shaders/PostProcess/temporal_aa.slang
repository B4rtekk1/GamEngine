[[vk::binding(0, 0)]] Sampler2D currentFrame;
[[vk::binding(1, 0)]] Sampler2D historyFrame;
[[vk::binding(2, 0)]] Sampler2D velocityFrame;
[[vk::binding(3, 0)]] Sampler2D currentDepthFrame;
[[vk::binding(4, 0)]] Sampler2D historyDepthFrame;
[[vk::binding(5, 0)]] Sampler2D waterVelocityFrame;
[[vk::binding(6, 0)]] Sampler2D waterMetaFrame;
[[vk::binding(7, 0)]] Sampler2D waterSurfaceFrame;
[[vk::binding(8, 0)]] RWTexture2D<float4> resolvedFrame;
[[vk::binding(9, 0)]] RWTexture2D<float> resolvedDepthFrame;
[[vk::binding(10, 0)]] RWTexture2D<float4> resolvedHistoryFrame;
struct Settings {
    float currentJitterX, currentJitterY, previousJitterX, previousJitterY;
    float historyWeight, inverseWidth, inverseHeight, padding;
    float projectionA, projectionB;
};
[[vk::push_constant]] ConstantBuffer<Settings> settings;
float3 reinhard(float3 color) { return color / (color + 1.0); }
float3 reinhardInverse(float3 color) { return color / max(1.0 - color, 1e-5); }
float viewDepth(float deviceDepth) {
    // Perspective RH_ZO: deviceDepth = -projectionA + projectionB / distance.
    return settings.projectionB / min(deviceDepth + settings.projectionA, -1e-6);
}

// Nine bilinear taps reconstruct the 4x4 Catmull-Rom footprint.
float3 sampleHistoryCatmullRom(float2 uv, float2 resolution) {
    float2 samplePos = uv * resolution;
    float2 texPos1 = floor(samplePos - 0.5) + 0.5;
    float2 f = samplePos - texPos1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 uv0 = (texPos1 - 1.0) / resolution;
    const float2 uv12 = (texPos1 + w2 / w12) / resolution;
    const float2 uv3 = (texPos1 + 2.0) / resolution;
    float3 cubic = 0.0;
    cubic += historyFrame.Sample(float2(uv0.x, uv0.y)).rgb * w0.x * w0.y;
    cubic += historyFrame.Sample(float2(uv12.x, uv0.y)).rgb * w12.x * w0.y;
    cubic += historyFrame.Sample(float2(uv3.x, uv0.y)).rgb * w3.x * w0.y;
    cubic += historyFrame.Sample(float2(uv0.x, uv12.y)).rgb * w0.x * w12.y;
    cubic += historyFrame.Sample(float2(uv12.x, uv12.y)).rgb * w12.x * w12.y;
    cubic += historyFrame.Sample(float2(uv3.x, uv12.y)).rgb * w3.x * w12.y;
    cubic += historyFrame.Sample(float2(uv0.x, uv3.y)).rgb * w0.x * w3.y;
    cubic += historyFrame.Sample(float2(uv12.x, uv3.y)).rgb * w12.x * w3.y;
    cubic += historyFrame.Sample(float2(uv3.x, uv3.y)).rgb * w3.x * w3.y;
    return max(cubic, 0.0);
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

[numthreads(16, 16, 1)] [shader("compute")]
void main(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID,
          uint3 dispatchId : SV_DispatchThreadID) {
    uint width, height;
    resolvedFrame.GetDimensions(width, height);
    const int2 extent = int2(width, height);
    const float2 resolution = float2(width, height);
    const float2 texel = 1.0 / resolution;
    if (any(dispatchId.xy >= uint2(extent))) return;
    // The resolve writes stable, unjittered pixels. Locate each output pixel's
    // corresponding sample in the jittered current render.
    const float2 outputUv = (float2(dispatchId.xy) + 0.5) * texel;
    const float2 currentJitter = float2(settings.currentJitterX,
                                        settings.currentJitterY) * 0.5;
    const float2 previousJitter = float2(settings.previousJitterX,
                                         settings.previousJitterY) * 0.5;
    const float2 jitterDelta = currentJitter - previousJitter;
    const float2 currentUv = clamp(outputUv - currentJitter, texel * 0.5, 1.0 - texel * 0.5);
    const float3 currentTm = reinhard(max(currentFrame.Sample(currentUv).rgb, 0.0));
    const uint waterQuality = settings.padding > 0.5 ? asuint(waterMetaFrame.Sample(currentUv).y) : 0u;
    const bool waterPixel = (waterQuality & 0x80000000u) != 0;
    const float currentDepth = waterPixel ? waterSurfaceFrame.Sample(currentUv).z
                                          : currentDepthFrame.Sample(currentUv).r;
    const float reactive = (waterQuality & 0x40000000u) != 0 ? 1.0 : 0.0;
    // At a silhouette, bilinear velocity may contain the background's motion
    // even though the current pixel belongs to the foreground.  Vulkan's
    // depth convention is near=0, so select the velocity at the nearest depth
    // in a compact 3x3 footprint before reprojection.
    float nearestDepth = currentDepth;
    float2 velocitySample = waterPixel ? waterVelocityFrame.Sample(currentUv).xy
                                       : velocityFrame.Sample(currentUv).xy;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            const float2 sampleUv = clamp(currentUv + float2(x, y) * texel, texel * 0.5, 1.0 - texel * 0.5);
            const uint sampleWaterQuality = settings.padding > 0.5 ? asuint(waterMetaFrame.Sample(sampleUv).y) : 0u;
            const bool sampleWater = (sampleWaterQuality & 0x80000000u) != 0;
            const float sampleDepth = sampleWater ? waterSurfaceFrame.Sample(sampleUv).z
                                                  : currentDepthFrame.Sample(sampleUv).r;
            if (sampleDepth < nearestDepth) {
                nearestDepth = sampleDepth;
                velocitySample = sampleWater ? waterVelocityFrame.Sample(sampleUv).xy
                                             : velocityFrame.Sample(sampleUv).xy;
            }
        }
    }

    // Velocity includes the jitter in both projection matrices. Subtract it
    // from the current UV to locate the corresponding previous-frame sample.
    // Remove jitter only when estimating actual scene motion.
    // Jitter is stored in NDC, while velocity is in UV space (NDC * 0.5).
    const float2 physicalVelocity = velocitySample + jitterDelta;
    const float2 historyUv = outputUv - physicalVelocity;
    const bool historyInBounds = all(historyUv >= 0.0) && all(historyUv <= 1.0);
    // Keep history depth linear before quantizing it to R16F. Device depth
    // loses too much precision near the far plane in a half-float image.
    const float currentViewDepth = viewDepth(currentDepth);
    const float historyViewDepth = historyDepthFrame.Sample(historyUv).r;
    const bool depthMatches = abs(currentViewDepth - historyViewDepth) <=
        0.02 * max(currentViewDepth, historyViewDepth);

    // Compute clipping statistics around the reconstructed current sample.
    float3 mean = 0.0;
    float3 meanSquared = 0.0;
    float totalWeight = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            const float2 sampleUv = clamp(currentUv + float2(x, y) * texel, texel * 0.5, 1.0 - texel * 0.5);
            const float3 sample = reinhard(max(currentFrame.Sample(sampleUv).rgb, 0.0));
            const float weight = exp(-3.0 * float(x * x + y * y) / 4.0);
            mean += sample * weight;
            meanSquared += sample * sample * weight;
            totalWeight += weight;
        }
    }
    mean /= totalWeight;
    meanSquared /= totalWeight;
    const float motionPixels = length(physicalVelocity * resolution);
    const float motionT = saturate(motionPixels / 32.0);
    const float boxSize = lerp(2.5, 0.5, motionT * motionT * (3.0 - 2.0 * motionT));
    const float3 deviation = sqrt(max(meanSquared - mean * mean, 0.0)) * boxSize;
    float3 history = sampleHistoryCatmullRom(historyUv, resolution);
    history = clipAabb(mean - deviation, mean + deviation, mean, history);

    const float historyWeight = settings.historyWeight * lerp(1.0, 0.12, reactive);
    const float3 resolvedTm = lerp(currentTm, history,
        (historyInBounds && depthMatches) ? historyWeight : 0.0);
    resolvedFrame[dispatchId.xy] = float4(reinhardInverse(resolvedTm), 1.0);
    resolvedDepthFrame[dispatchId.xy] = currentViewDepth;
    resolvedHistoryFrame[dispatchId.xy] = float4(resolvedTm, 1.0);
}
