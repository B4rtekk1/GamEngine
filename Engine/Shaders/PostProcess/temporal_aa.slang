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
// Linear view depth of the resolved frame, so the resolved colour doubles as
// the next history.
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

// Five bilinear taps reconstruct the 4x4 Catmull-Rom footprint. The four
// corner taps carry at most ~0.5% of the weight each (negative lobe squared);
// dropping them and renormalizing is visually identical and saves four
// history fetches per pixel.
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
    const float weightCenter = w12.x * w12.y;
    const float weightUp = w12.x * w0.y;
    const float weightDown = w12.x * w3.y;
    const float weightLeft = w0.x * w12.y;
    const float weightRight = w3.x * w12.y;
    float3 cubic = historyTap(float2(uv12.x, uv12.y)) * weightCenter;
    cubic += historyTap(float2(uv12.x, uv0.y)) * weightUp;
    cubic += historyTap(float2(uv12.x, uv3.y)) * weightDown;
    cubic += historyTap(float2(uv0.x, uv12.y)) * weightLeft;
    cubic += historyTap(float2(uv3.x, uv12.y)) * weightRight;
    cubic /= weightCenter + weightUp + weightDown + weightLeft + weightRight;
    // Anti-ringing: the negative lobes overshoot the 2x2 texels around the
    // sample at every high-contrast edge. Near white the overshoot reaches
    // 1.0 in Reinhard space, which reinhardInverse turns into a MaxHdr white
    // dot, and the static clip box (mean +- 2.5 sigma) is wide enough to let
    // it through and feed it back into history.
    const int2 lastTexel = int2(resolution) - 1;
    const int2 baseTexel = int2(texPos1 - 0.5);
    float3 footprintMin = 1.0;
    float3 footprintMax = 0.0;
    [unroll] for (int y = 0; y <= 1; ++y) {
        [unroll] for (int x = 0; x <= 1; ++x) {
            const int3 texelCoord = int3(clamp(baseTexel + int2(x, y), int2(0, 0), lastTexel), 0);
            const float3 tap = reinhard(min(max(historyFrame.Load(texelCoord).rgb, 0.0), MaxHdr));
            footprintMin = min(footprintMin, tap);
            footprintMax = max(footprintMax, tap);
        }
    }
    return clamp(cubic, footprintMin, footprintMax);
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
        // Exact texels: no bilinear blur of the current frame. Load skips
        // the sampler and UV arithmetic for all 324 halo fetches.
        const int3 texelCoord = int3(clamp(tilePixel, int2(0, 0), extent - 1), 0);
        const float4 raw = currentFrame.Load(texelCoord);
        const float3 sample = reinhard(min(max(raw.rgb, 0.0), MaxHdr));
        const uint quality = settings.padding > 0.5 ? asuint(waterMetaFrame.Load(texelCoord).y) : 0u;
        tileRed[index] = sample.r;
        tileGreen[index] = sample.g;
        tileBlue[index] = sample.b;
        tileQuality[index] = quality;
        tileDepth[index] = (quality & 0x80000000u) != 0 ? waterSurfaceFrame.Load(texelCoord).z
                                                        : currentDepthFrame.Load(texelCoord).r;
    }
    // Partial workgroups must populate the halo and reach the barrier too.
    GroupMemoryBarrierWithGroupSync();
    if (any(dispatchId.xy >= uint2(extent))) return;
    const uint centerIndex = (threadId.y + 1u) * 18u + threadId.x + 1u;
    const uint waterQuality = tileQuality[centerIndex];
    const float currentDepth = tileDepth[centerIndex];
    const float reactive = (waterQuality & 0x40000000u) != 0 ? 1.0 : 0.0;

    // Reconstruct the current colour with a Gaussian (Blackman-Harris-like)
    // weight around the jittered sample positions, and gather the neighbourhood
    // statistics used for clipping, and the nearest-depth sample used for motion
    // (at a silhouette bilinear velocity may belong to the background; Vulkan
    // depth convention is near=0).
    const float2 jitterPixels = currentJitter * resolution;
    float3 mean = 0.0;
    float3 meanSquared = 0.0;
    float3 neighbourhoodMin = 1.0;
    float3 neighbourhoodMax = 0.0;
    float totalWeight = 0.0;
    float3 filtered = 0.0;
    float filterWeight = 0.0;
    float nearestDepth = currentDepth;
    int2 nearestOffset = int2(0, 0);
    // The Gaussian is separable: three exp per axis instead of nine.
    float reconstructionX[3];
    float reconstructionY[3];
    [unroll] for (int i = 0; i < 3; ++i) {
        const float2 d = float(i - 1) + jitterPixels;
        reconstructionX[i] = exp(-3.2 * d.x * d.x);
        reconstructionY[i] = exp(-3.2 * d.y * d.y);
    }
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            const uint index = uint(int(centerIndex) + y * 18 + x);
            const float3 sample = float3(tileRed[index], tileGreen[index], tileBlue[index]);
            const float weight = exp(-3.0 * float(x * x + y * y) / 4.0);
            mean += sample * weight;
            meanSquared += sample * sample * weight;
            neighbourhoodMin = min(neighbourhoodMin, sample);
            neighbourhoodMax = max(neighbourhoodMax, sample);
            totalWeight += weight;
            const float reconstruction = reconstructionX[x + 1] * reconstructionY[y + 1];
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
    const int3 nearestTexel = int3(clamp(int2(dispatchId.xy) + nearestOffset, int2(0, 0), extent - 1), 0);
    // Fetch motion only for the winning depth sample.
    const float2 velocitySample = nearestWater ? waterVelocityFrame.Load(nearestTexel).xy
                                               : velocityFrame.Load(nearestTexel).xy;

    // Velocity includes the jitter in both projection matrices. Subtract it
    // from the current UV to locate the corresponding previous-frame sample.
    // Remove jitter only when estimating actual scene motion.
    // Jitter is stored in NDC, while velocity is in UV space (NDC * 0.5).
    // (The projection jitter moves the image by minus its value, so a static
    // pixel's velocity is -jitterDelta and this sum is zero.)
    const float2 physicalVelocity = velocitySample + jitterDelta;
    const float2 historyUv = outputUv - physicalVelocity;
    const bool historyInBounds = all(historyUv >= 0.0) && all(historyUv <= 1.0);
    // Keep history depth linear before quantizing it to R16F. Device depth
    // loses too much precision near the far plane in a half-float image.
    // Compare the nearest depth of the 3x3 neighbourhood, not the centre
    // sample. Jitter moves a silhouette pixel's centre between the object
    // and its background every frame; testing that sample rejected the
    // history along every edge, and the raw aliased sample flickered there.
    // The neighbourhood minimum is the same whichever side the jitter hits.
    const float currentViewDepth = viewDepth(nearestDepth);
    const float resolvedDepth = currentViewDepth;
    const float historyDepthSample = historyDepthFrame.SampleLevel(historyUv, 0.0).r;
    const float historyViewDepth = abs(historyDepthSample);
    // Soft disocclusion: full weight inside the tolerance, fading to zero at
    // twice the tolerance instead of a hard cut that flickers on edges.
    const float depthTolerance = 0.02 * max(max(currentViewDepth, historyViewDepth), 1e-4);
    // Motion is measured at the nearest-depth sample, so a pixel next to a
    // moving object reads that object's velocity.
    const float motionPixels = length(physicalVelocity * resolution);
    const float motionT = saturate(motionPixels / 8.0);
    const float clipT = saturate(motionPixels / 2.0);
    const float clipBlend = clipT * clipT * (3.0 - 2.0 * clipT);
    // A sub-pixel wire or leaf is missed by some jitter offsets and then
    // vanishes from the whole 3x3 depth neighbourhood. To this test that is
    // indistinguishable from a disocclusion, so a static thin feature lost
    // its history every few frames and flickered strongly. Static pixels
    // therefore rely on colour clipping alone; once anything moves (the
    // camera, or an object whose velocity the nearest-depth sample picks up
    // at its silhouette) the depth test applies in full.
    const float depthWeight = lerp(1.0, saturate(2.0 - abs(currentViewDepth - historyViewDepth) / depthTolerance),
                                   clipBlend);
    if (!historyInBounds || depthWeight <= 0.0 || settings.historyWeight <= 0.0) {
        resolvedFrame[dispatchId.xy] = float4(min(reinhardInverse(currentTm), MaxHdr), 1.0);
        resolvedDepthFrame[dispatchId.xy] = resolvedDepth;
        return;
    }

    // Two failure modes pull the clip box in opposite directions. In motion,
    // a wide variance box let stale history through, and the Catmull-Rom
    // overshoot at edges accumulated into bright rims and smeared texture:
    // there the box stays near one deviation and inside the neighbourhood's
    // actual range. At rest, jitter alone decides whether a sub-pixel wire or
    // leaf appears in this frame's 3x3 neighbourhood; a tight box (and the
    // range limit in particular) then removed it from history every other
    // frame and it flickered. Static pixels keep the original wide box.
    const float boxSize = lerp(2.5, 0.75, clipBlend);
    const float3 deviation = sqrt(max(meanSquared - mean * mean, 0.0)) * boxSize;
    const float3 boxMin = max(mean - deviation, lerp(mean - deviation, neighbourhoodMin, clipBlend));
    const float3 boxMax = min(mean + deviation, lerp(mean + deviation, neighbourhoodMax, clipBlend));
    const float3 unclippedHistory = sampleHistoryCatmullRom(historyUv, resolution);
    float3 history = clipAabb(boxMin, boxMax, clamp(mean, boxMin, boxMax), unclippedHistory);
    // Pixels that do not move keep their history unclipped. A detail thinner
    // than a pixel (hanging flowers, string-light wires, a lamp's cage) is
    // hit by only some of the 16 jitter offsets; in the other frames this
    // frame's 3x3 neighbourhood does not contain it at all, any clip box
    // built from it collapses to the background, and the detail was erased
    // from history and redrawn in step with the jitter sequence: a visible
    // 16-frame flicker on every bright thin feature. A static pixel's history
    // is already the right average of those offsets. The cost is that a
    // lighting change on static geometry fades in over the history length
    // (~16 frames) instead of at once; anything that moves clips as before.
    const float staticFactor = 1.0 - smoothstep(0.02, 0.1, motionPixels);
    history = lerp(history, unclippedHistory, staticFactor);

    // Constant feedback keeps the accumulation long enough to resolve sub-pixel
    // edges; only water and soft disocclusions shorten it.
    float historyWeight = settings.historyWeight;
    historyWeight *= lerp(1.0, 0.12, reactive);
    historyWeight *= depthWeight;
    // Each resampled frame adds a little reprojection blur; let moving
    // pixels refresh faster so it cannot build up.
    historyWeight *= lerp(1.0, 0.92, motionT);
    const float3 resolvedTm = lerp(currentTm, history, historyWeight);
    const float3 resolvedLinear = min(reinhardInverse(resolvedTm), MaxHdr);
    resolvedFrame[dispatchId.xy] = float4(resolvedLinear, 1.0);
    resolvedDepthFrame[dispatchId.xy] = resolvedDepth;
}
