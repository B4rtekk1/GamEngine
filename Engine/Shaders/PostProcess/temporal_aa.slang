// Compute resolve operates on single-mip render targets; explicit LOD avoids compute derivatives.
[[vk::binding(0, 0)]] Sampler2D currentFrame;
[[vk::binding(1, 0)]] Sampler2D historyFrame;
[[vk::binding(2, 0)]] Sampler2D velocityFrame;
[[vk::binding(3, 0)]] Sampler2D currentDepthFrame;
[[vk::binding(4, 0)]] Sampler2D historyDepthFrame;
[[vk::binding(5, 0)]] Sampler2D waterVelocityFrame;
[[vk::binding(6, 0)]] Sampler2D waterMetaFrame;
[[vk::binding(7, 0)]] Sampler2D waterSurfaceFrame;
[[vk::binding(8, 0)]] RWTexture2D<float4> resolvedFrame;
// Linear view depth of the resolved frame. Its sign carries the
// glass-reactive flag, so the resolved colour doubles as the next history.
[[vk::binding(9, 0)]] RWTexture2D<float> resolvedDepthFrame;
struct Settings {
    float currentJitterX, currentJitterY, previousJitterX, previousJitterY;
    float historyWeight, inverseWidth, inverseHeight, padding;
    float projectionA, projectionB;
};
[[vk::push_constant]] ConstantBuffer<Settings> settings;
// A 16x16 output tile and a one-pixel halo share the current-frame samples
// (colour, effective depth and water metadata) used by the reconstruction
// filter, clipping statistics and the nearest-depth motion search. Store
// channels separately so adjacent lanes access adjacent shared-memory words.
groupshared float tileRed[18 * 18];
groupshared float tileGreen[18 * 18];
groupshared float tileBlue[18 * 18];
groupshared float tileAlpha[18 * 18];
groupshared float tileDepth[18 * 18];
groupshared uint tileQuality[18 * 18];
// Largest finite FP16 value; keeps linear history from overflowing to inf.
static const float MaxHdr = 65000.0;
float3 reinhard(float3 color) { return color / (color + 1.0); }
float3 reinhardInverse(float3 color) { return color / max(1.0 - color, 1e-6); }
float viewDepth(float deviceDepth) {
    // Perspective RH_ZO: deviceDepth = -projectionA + projectionB / distance.
    return settings.projectionB / min(deviceDepth + settings.projectionA, -1e-6);
}

// History is stored as linear HDR (FP16 keeps relative precision for very
// bright pixels, unlike a Reinhard-compressed value close to 1). Tonemap
// each tap so clipping still happens in the compressed domain.
float3 historyTap(float2 uv) {
    return reinhard(min(max(historyFrame.SampleLevel(uv, 0.0).rgb, 0.0), MaxHdr));
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
    cubic += historyTap(float2(uv0.x, uv0.y)) * w0.x * w0.y;
    cubic += historyTap(float2(uv12.x, uv0.y)) * w12.x * w0.y;
    cubic += historyTap(float2(uv3.x, uv0.y)) * w3.x * w0.y;
    cubic += historyTap(float2(uv0.x, uv12.y)) * w0.x * w12.y;
    cubic += historyTap(float2(uv12.x, uv12.y)) * w12.x * w12.y;
    cubic += historyTap(float2(uv3.x, uv12.y)) * w3.x * w12.y;
    cubic += historyTap(float2(uv0.x, uv3.y)) * w0.x * w3.y;
    cubic += historyTap(float2(uv12.x, uv3.y)) * w12.x * w3.y;
    cubic += historyTap(float2(uv3.x, uv3.y)) * w3.x * w3.y;
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
    // The resolve writes stable, unjittered pixels. Locate each output pixel's
    // corresponding sample in the jittered current render.
    const float2 outputUv = (float2(dispatchId.xy) + 0.5) * texel;
    const float2 currentJitter = float2(settings.currentJitterX,
                                        settings.currentJitterY) * 0.5;
    const float2 previousJitter = float2(settings.previousJitterX,
                                         settings.previousJitterY) * 0.5;
    const float2 jitterDelta = currentJitter - previousJitter;
    const uint lane = threadId.y * 16u + threadId.x;
    for (uint index = lane; index < 18u * 18u; index += 256u) {
        const int2 tilePixel = int2(groupId.xy) * 16 + int2(index % 18u, index / 18u) - 1;
        // Exact texel centres: no bilinear blur of the current frame.
        const int2 clampedPixel = clamp(tilePixel, int2(0, 0), extent - 1);
        const float2 sampleUv = (float2(clampedPixel) + 0.5) * texel;
        const float4 raw = currentFrame.SampleLevel(sampleUv, 0.0);
        const float3 sample = reinhard(min(max(raw.rgb, 0.0), MaxHdr));
        const uint quality = settings.padding > 0.5 ? asuint(waterMetaFrame.SampleLevel(sampleUv, 0.0).y) : 0u;
        tileRed[index] = sample.r;
        tileGreen[index] = sample.g;
        tileBlue[index] = sample.b;
        tileAlpha[index] = raw.a;
        tileQuality[index] = quality;
        tileDepth[index] = (quality & 0x80000000u) != 0 ? waterSurfaceFrame.SampleLevel(sampleUv, 0.0).z
                                                        : currentDepthFrame.SampleLevel(sampleUv, 0.0).r;
    }
    // Partial workgroups must populate the halo and reach the barrier too.
    GroupMemoryBarrierWithGroupSync();
    if (any(dispatchId.xy >= uint2(extent))) return;
    const uint centerIndex = (threadId.y + 1u) * 18u + threadId.x + 1u;
    const uint waterQuality = tileQuality[centerIndex];
    const float currentDepth = tileDepth[centerIndex];
    const float glassReactive = saturate(-tileAlpha[centerIndex]);
    const float reactive = max(glassReactive, (waterQuality & 0x40000000u) != 0 ? 1.0 : 0.0);

    // Reconstruct the current colour with a Gaussian (Blackman-Harris-like)
    // weight around the jittered sample positions, and gather the neighbourhood
    // statistics used for clipping, and the nearest-depth sample used for motion
    // (at a silhouette bilinear velocity may belong to the background; Vulkan
    // depth convention is near=0).
    const float2 jitterPixels = currentJitter * resolution;
    float3 mean = 0.0;
    float3 meanSquared = 0.0;
    float totalWeight = 0.0;
    float3 filtered = 0.0;
    float filterWeight = 0.0;
    float nearestDepth = currentDepth;
    int2 nearestOffset = int2(0, 0);
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            const uint index = uint(int(centerIndex) + y * 18 + x);
            const float3 sample = float3(tileRed[index], tileGreen[index], tileBlue[index]);
            const float weight = exp(-3.0 * float(x * x + y * y) / 4.0);
            mean += sample * weight;
            meanSquared += sample * sample * weight;
            totalWeight += weight;
            const float2 d = float2(x, y) + jitterPixels;
            const float reconstruction = exp(-3.2 * dot(d, d));
            filtered += sample * reconstruction;
            filterWeight += reconstruction;
            const float sampleDepth = tileDepth[index];
            if (sampleDepth < nearestDepth) {
                nearestDepth = sampleDepth;
                nearestOffset = int2(x, y);
            }
        }
    }
    mean /= totalWeight;
    meanSquared /= totalWeight;
    const float3 currentTm = filtered / filterWeight;
    const uint nearestIndex = uint(int(centerIndex) + nearestOffset.y * 18 + nearestOffset.x);
    const bool nearestWater = (tileQuality[nearestIndex] & 0x80000000u) != 0;
    const int2 nearestPixel = clamp(int2(dispatchId.xy) + nearestOffset, int2(0, 0), extent - 1);
    const float2 nearestUv = (float2(nearestPixel) + 0.5) * texel;
    // Fetch motion only for the winning depth sample.
    const float2 velocitySample = nearestWater ? waterVelocityFrame.SampleLevel(nearestUv, 0.0).xy
                                               : velocityFrame.SampleLevel(nearestUv, 0.0).xy;

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
    const float resolvedDepth = glassReactive > 0.0 ? -currentViewDepth : currentViewDepth;
    const float historyDepthSample = historyDepthFrame.SampleLevel(historyUv, 0.0).r;
    const float historyViewDepth = abs(historyDepthSample);
    // Soft disocclusion: full weight inside the tolerance, fading to zero at
    // twice the tolerance instead of a hard cut that flickers on edges.
    const float depthTolerance = 0.02 * max(max(currentViewDepth, historyViewDepth), 1e-4);
    const float depthWeight = saturate(2.0 - abs(currentViewDepth - historyViewDepth) / depthTolerance);
    if (!historyInBounds || depthWeight <= 0.0 || settings.historyWeight <= 0.0) {
        resolvedFrame[dispatchId.xy] = float4(min(reinhardInverse(currentTm), MaxHdr), 1.0);
        resolvedDepthFrame[dispatchId.xy] = resolvedDepth;
        return;
    }

    const float motionPixels = length(physicalVelocity * resolution);
    const float motionT = saturate(motionPixels / 32.0);
    const float boxSize = lerp(2.5, 0.5, motionT * motionT * (3.0 - 2.0 * motionT));
    const float3 deviation = sqrt(max(meanSquared - mean * mean, 0.0)) * boxSize;
    float3 history = sampleHistoryCatmullRom(historyUv, resolution);
    history = clipAabb(mean - deviation, mean + deviation, mean, history);

    const float previousGlassReactive = historyDepthSample < 0.0 ? 1.0 : 0.0;
    // Constant feedback keeps the accumulation long enough to resolve sub-pixel
    // edges; only glass/water and soft disocclusions shorten it.
    float historyWeight = settings.historyWeight;
    historyWeight *= lerp(1.0, 0.12, max(reactive, previousGlassReactive));
    historyWeight *= depthWeight;
    const float3 resolvedTm = lerp(currentTm, history, historyWeight);
    const float3 resolvedLinear = min(reinhardInverse(resolvedTm), MaxHdr);
    resolvedFrame[dispatchId.xy] = float4(resolvedLinear, 1.0);
    resolvedDepthFrame[dispatchId.xy] = resolvedDepth;
}
