// Ray-traced shadows of point and spot lights. For every traced pixel this
// walks the same cluster light list forward shading will (the first 32
// entries), ranks the lights by unshadowed irradiance and traces one ray to
// each of the strongest few. It writes their global light indices with one
// binary visibility sample each; local_light_shadow_denoise turns those into
// the soft occlusion forward shading reads.
//
// Two builds of this file: a compute kernel tracing inline ray queries
// against the classic triangle TLAS, and (LOCAL_SHADOW_RAYGEN) a ray tracing
// pipeline tracing the Mega Geometry cluster scene, whose partitioned TLAS
// and cluster BLASes only ray tracing pipelines may traverse.
import Common.SceneData;

#ifndef LOCAL_SHADOW_RAYGEN
#define LOCAL_SHADOW_RAYGEN 0
#endif

#if !LOCAL_SHADOW_RAYGEN
[[vk::binding(0, 0)]] RaytracingAccelerationStructure sceneTlas;
#endif
[[vk::binding(1, 0)]] Sampler2D depthTexture;
[[vk::binding(2, 0)]] Sampler2D viewNormalTexture;
// Four 16-bit slots in x (0, 1) and y (2, 3): bits 0-9 light index, bit 14
// occluded, bit 13 ranked but not traced this frame (history reuse), bit 15
// slot valid. Zero: no receiver. One texel per 2x2 quad.
[[vk::binding(3, 0), vk::image_format("rg32ui")]] RWTexture2D<uint2> shadowMask;
[[vk::binding(4, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(5, 0)]] StructuredBuffer<ClusterLightRange> clusterRanges;
[[vk::binding(6, 0)]] StructuredBuffer<uint> clusterLightIndices;
[[vk::binding(7, 0)]] StructuredBuffer<LocalLightData> localLights;
// Last frame's denoised result (local_light_shadow_denoise.slang).
[[vk::binding(8, 0)]] Sampler2D<uint4> denoisedHistory;
// Stochastic light sampling (mode 1, MegaLights-style): the depth-bounded
// tile lists, and per quad the four samples' RIS weights as half floats
// (x: samples 0 | 1 << 16, y: 2 | 3 << 16).
[[vk::binding(9, 0)]] StructuredBuffer<uint> tileLightIndices;
[[vk::binding(10, 0), vk::image_format("rg32ui")]] RWTexture2D<uint2> sampleWeights;

struct LocalShadowSettings
{
    // Rays per pixel (at most 4), to its strongest lights; the rest stay unshadowed.
    uint maxRays;
    float normalBias;
    // Lights whose unshadowed irradiance at the receiver is below this skip
    // the ray: their shadow would not be visible.
    float minIrradiance;
    // A quad whose denoised history has converged traces only every this many
    // frames (1: always); geometry and lights are static, so its visibility is.
    uint convergedInterval;
    // Partitioned TLAS of the cluster scene, bound by address (raygen build).
    uint64_t sceneAddress;
    // 0: rays to the strongest lights, for the visibility denoiser.
    // 1: four lights sampled in proportion to their unshadowed irradiance
    //    from the tile list, for stochastic shading (deferred_local_lights).
    uint mode;
    uint padding;
};
[[vk::push_constant]] LocalShadowSettings settings;

struct ShadowPayload { uint hit; };

// Any hit along the segment occludes; nothing is shaded.
bool occluded(RayDesc ray)
{
#if LOCAL_SHADOW_RAYGEN
    ShadowPayload payload = { 1u };
    TraceRay(RaytracingAccelerationStructure(settings.sceneAddress),
             RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_FORCE_OPAQUE, 0xff,
             0, 0, 0, ray, payload);
    return payload.hit != 0u;
#else
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(sceneTlas,
                         RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
                             RAY_FLAG_FORCE_OPAQUE,
                         0x01, ray);
    query.Proceed();
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
#endif
}

float3 decodeOct(float2 e)
{
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * select(n.xy >= 0.0, 1.0, -1.0);
    return normalize(n);
}

// Lamps are not points: aim each frame's ray at a different point of a small
// emitter. The denoiser averages the binary results into a soft penumbra.
static const float LightSourceRadius = 0.15;

float3 sourceOffset(uint2 pixel, uint frameIndex, uint entry)
{
    // Interleaved gradient noise per pixel, advanced along R2 each frame.
    const float ign = frac(52.9829189 * frac(dot(float2(pixel), float2(0.06711056, 0.00583715))));
    const float2 u = frac(ign + float(frameIndex % 64u + entry * 7u) * float2(0.754877666, 0.569840296));
    // Uniform direction on the sphere, scaled into its volume.
    const float z = u.x * 2.0 - 1.0;
    const float phi = u.y * 6.28318530718;
    const float r = sqrt(max(1.0 - z * z, 0.0));
    return float3(r * cos(phi), z, r * sin(phi)) * (LightSourceRadius * sqrt(u.x * (1.0 - u.x) * 4.0));
}

float3 reconstructViewPosition(int2 pixel, float depth, int2 extent)
{
    const float2 uv = (float2(pixel) + 0.5) / float2(extent);
    const float4 position = mul(float4(uv * 2.0 - 1.0, depth, 1.0), frame.inverseProjection);
    return position.xyz / position.w;
}

// One pixel of each 2x2 quad traces, rotating through the quad over four
// frames, into the quad's texel. The denoiser's spatial and temporal filters
// and forward shading's depth-aware upsample fold this back to full resolution.
static const int2 QuadOrder[4] = { int2(0, 0), int2(1, 1), int2(1, 0), int2(0, 1) };

float hashToUnit(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return float(value >> 8u) / 16777216.0;
}

// Resampled importance sampling over the tile's lights: four independent
// reservoirs stream the list once, each keeping a light with probability
// proportional to its unshadowed irradiance here (the target p^). A sample's
// weight W = sum(p^) / p^(sample) makes f * V * W / 4 an unbiased estimate of
// the lighting, whatever the light count; shadow rays go only to the samples.
void sampleLights(uint2 id, int2 pixel, int2 extent, float3 viewPosition, float3 worldPosition, float3 worldNormal)
{
    const uint tilesX = (uint(extent.x) + TileLightPixelSize - 1u) / TileLightPixelSize;
    const uint2 tile = uint2(pixel) / TileLightPixelSize;
    const uint tileBase = (tile.y * tilesX + tile.x) * TileLightStride;
    const uint listCount = tileLightIndices[tileBase];
    const float viewDepth = -viewPosition.z;
    const float bias = settings.normalBias + viewDepth * 2.0e-4;
    const float3 origin = worldPosition + worldNormal * bias;
    const uint seed = (id.x * 73856093u) ^ (id.y * 19349663u) ^ (frame.temporalSampleIndex * 83492791u);
    float weightSum = 0.0;
    uint4 chosen = uint4(0xffffffffu);
    float4 chosenTarget = 0.0;
    for (uint entry = 0u; entry < listCount; ++entry)
    {
        const uint lightIndex = tileLightIndices[tileBase + 1u + entry];
        if (lightIndex >= frame.localLightCount) continue;
        const LocalLightData light = localLights[lightIndex];
        const float3 toLight = light.positionRange.xyz - origin;
        const float distanceSquared = dot(toLight, toLight);
        const float radius = light.positionRange.w;
        if (distanceSquared <= 1e-6 || distanceSquared >= radius * radius) continue;
        const float distanceToLight = sqrt(distanceSquared);
        const float3 direction = toLight / distanceToLight;
        const float window = saturate(1.0 - distanceToLight / radius);
        // N.L from the geometric normal, floored: shading uses mapped and
        // wrapped normals, which a zero target would leave unsampled (bias).
        const float nDotL = saturate(dot(worldNormal, direction)) * 0.9 + 0.1;
        float target = dot(light.colorIntensity.rgb, float3(0.2126, 0.7152, 0.0722)) * light.colorIntensity.w *
                       window * window * nDotL / max(distanceSquared, LocalLightSourceRadiusSquared);
        target *= localLightProfile(light, -direction);
        if (light.parameters.y > 1.5) // Spot
            target *= smoothstep(light.directionOuterCos.w, light.parameters.x,
                                 dot(light.directionOuterCos.xyz, -direction));
        if (target <= 0.0) continue;
        weightSum += target;
        const float keep = target / weightSum;
        [unroll] for (uint k = 0u; k < 4u; ++k)
        {
            if (hashToUnit(seed + entry * 4u + k * 0x9e3779b9u) < keep)
            {
                chosen[k] = lightIndex;
                chosenTarget[k] = target;
            }
        }
    }
    uint slots[4];
    float weights[4];
    [unroll] for (uint k = 0u; k < 4u; ++k)
    {
        slots[k] = 0u;
        weights[k] = 0.0;
        if (chosen[k] == 0xffffffffu) continue;
        const LocalLightData light = localLights[chosen[k]];
        bool blocked = false;
        if (light.parameters.z >= 0.5) // casts shadows
        {
            const float3 toLight = light.positionRange.xyz + sourceOffset(id, frame.temporalSampleIndex, k) - origin;
            const float distanceToLight = length(toLight);
            RayDesc ray;
            ray.Origin = origin;
            ray.Direction = toLight / distanceToLight;
            ray.TMin = 0.001;
            ray.TMax = max(distanceToLight - 0.6, 0.0);
            blocked = occluded(ray);
        }
        slots[k] = 0x8000u | (blocked ? 0x4000u : 0u) | (chosen[k] & 0x3ffu);
        weights[k] = min(weightSum / chosenTarget[k], 65000.0); // half float range
    }
    shadowMask[id.xy] = uint2(slots[0] | (slots[1] << 16u), slots[2] | (slots[3] << 16u));
    sampleWeights[id.xy] = uint2(f32tof16(weights[0]) | (f32tof16(weights[1]) << 16u),
                                 f32tof16(weights[2]) | (f32tof16(weights[3]) << 16u));
}

void traceQuad(uint2 id)
{
    uint quadWidth, quadHeight, width, height;
    shadowMask.GetDimensions(quadWidth, quadHeight);
    depthTexture.GetDimensions(width, height);
    if (id.x >= quadWidth || id.y >= quadHeight) return;
    const int2 quad = int2(id.xy) * 2;
    const int2 extent = int2(width, height);
    const int2 pixel = min(quad + QuadOrder[frame.temporalSampleIndex & 3u], extent - 1);
    const float depth = depthTexture.Load(int3(pixel, 0)).x;
    if (depth >= 0.999999 || frame.localLightCount == 0u)
    {
        shadowMask[id.xy] = uint2(0u, 0u);
        return;
    }
    const float3 viewPosition = reconstructViewPosition(pixel, depth, extent);
    const float3 worldPosition = mul(float4(viewPosition, 1.0), frame.inverseView).xyz;
    const float3 viewNormal = decodeOct(viewNormalTexture.Load(int3(pixel, 0)).xy);
    const float3 worldNormal = normalize(mul(float4(viewNormal, 0.0), frame.inverseView).xyz);

    // History reuse: where last frame's denoised result already holds this
    // surface with a long history, staggered quads skip their rays and the
    // denoiser keeps the accumulated value.
    bool converged = false;
    bool reuseTurn = true;
    uint4 historyTexel = uint4(0u, 0u, 0u, 0u);
    if (settings.convergedInterval > 1u)
    {
        const float4 previousView = mul(float4(worldPosition, 1.0), frame.previousView);
        const float4 previousClip = mul(previousView, frame.previousProjection);
        if (previousClip.w > 0.0)
        {
            const float2 previousUv = previousClip.xy / previousClip.w * 0.5 + 0.5;
            const int2 previousQuad = int2(floor(previousUv * float2(extent))) / 2;
            if (all(previousUv >= 0.0) && all(previousQuad < int2(quadWidth, quadHeight)))
            {
                historyTexel = denoisedHistory.Load(int3(previousQuad, 0));
                const uint packed = historyTexel.w;
                const float expectedDepth = -previousView.z / previousView.w;
                converged = (packed & 0xffu) >= 24u &&
                            abs(asfloat(packed & ~0xffu) - expectedDepth) < 0.05 * expectedDepth + 0.03;
                const uint phase = (id.x & 1u) + 2u * (id.y & 1u) + (id.x >> 1u) * 3u + (id.y >> 1u) * 5u;
                // Whole runs of four frames, so the quad still visits all
                // four of its pixels (the rotation has period four too).
                reuseTurn = (phase + (frame.temporalSampleIndex >> 2u)) % settings.convergedInterval == 0u;
            }
        }
    }

    if (settings.mode == 1u)
    {
        sampleLights(id, pixel, extent, viewPosition, worldPosition, worldNormal);
        return;
    }

    // The same cluster lookup as forward_pbr.
    const uint2 grid = (uint2(frame.clusteredViewportNearFar.xy) + ClusterTilePixelSize - 1u) / ClusterTilePixelSize;
    const float viewDepth = max(-viewPosition.z, frame.clusteredViewportNearFar.z);
    const uint slice = min(uint(max(log2(viewDepth) * frame.clusterZScaleBiasEnvironmentMipRtContact.x +
                                        frame.clusterZScaleBiasEnvironmentMipRtContact.y, 0.0)), 23u);
    const uint2 tile = min(uint2(pixel) / ClusterTilePixelSize, grid - 1u);
    const uint clusterIndex = (slice * grid.y + tile.y) * grid.x + tile.x;
    const ClusterLightRange range = clusterRanges[clusterIndex];

    // Offset along the normal by the depth quantization at this distance as
    // well as the user bias, so the receiver does not shadow itself.
    const float bias = settings.normalBias + viewDepth * 2.0e-4;
    const float3 origin = worldPosition + worldNormal * bias;
    // Rays go only to the strongest lights at this receiver: their shadows
    // are the visible ones, while a dim light's leak is not. Rank every
    // candidate by its unshadowed irradiance first, then trace the best few.
    static const uint MaxTracedLights = 4u;
    float bestIrradiance[MaxTracedLights];
    uint bestEntry[MaxTracedLights];
    [unroll] for (uint k = 0u; k < MaxTracedLights; ++k)
    {
        bestIrradiance[k] = 0.0;
        bestEntry[k] = 0xffffffffu;
    }
    const uint tracedLights = min(settings.maxRays, MaxTracedLights);
    const uint listCount = min(range.lightCount, 32u);
    for (uint i = 0u; i < listCount; ++i)
    {
        const uint lightIndex = clusterLightIndices[range.lightOffset + i];
        if (lightIndex >= frame.localLightCount) continue;
        const LocalLightData light = localLights[lightIndex];
        if (light.parameters.z < 0.5) continue; // does not cast shadows
        const float3 toLight = light.positionRange.xyz - origin;
        const float distanceSquared = dot(toLight, toLight);
        const float radius = light.positionRange.w;
        if (distanceSquared <= 1e-6 || distanceSquared >= radius * radius) continue;
        const float distanceToLight = sqrt(distanceSquared);
        const float3 direction = toLight / distanceToLight;
        const float nDotL = dot(worldNormal, direction);
        if (nDotL <= 0.0) continue;
        const float window = saturate(1.0 - distanceToLight / radius);
        float irradiance = light.colorIntensity.w * window * window * nDotL / max(distanceSquared, LocalLightSourceRadiusSquared);
        irradiance *= localLightProfile(light, -direction);
        if (light.parameters.y > 1.5) // Spot
            irradiance *= smoothstep(light.directionOuterCos.w, light.parameters.x,
                                     dot(light.directionOuterCos.xyz, -direction));
        if (irradiance < settings.minIrradiance) continue;
        // Insert into the descending top list; a displaced entry moves down.
        float candidateIrradiance = irradiance;
        uint candidateEntry = i;
        [unroll] for (uint k = 0u; k < MaxTracedLights; ++k)
        {
            if (k < tracedLights && candidateIrradiance > bestIrradiance[k])
            {
                const float keptIrradiance = bestIrradiance[k];
                const uint keptEntry = bestEntry[k];
                bestIrradiance[k] = candidateIrradiance;
                bestEntry[k] = candidateEntry;
                candidateIrradiance = keptIrradiance;
                candidateEntry = keptEntry;
            }
        }
    }
    // Converged only if the history holds every light ranked here; a light
    // entering the set needs full-rate samples to start its accumulation.
    if (converged)
    {
        [unroll] for (uint k = 0u; k < MaxTracedLights; ++k)
        {
            if (bestEntry[k] == 0xffffffffu) continue;
            const uint lightIndex = clusterLightIndices[range.lightOffset + bestEntry[k]];
            const bool present = (historyTexel.x & 0xffffu) == lightIndex || (historyTexel.x >> 16u) == lightIndex ||
                                 (historyTexel.y & 0xffffu) == lightIndex || (historyTexel.y >> 16u) == lightIndex;
            converged = converged && present;
        }
    }
    // Trace in cluster-list order, not irradiance order: neighbouring pixels
    // share the list but rank nearly equal lights differently, so a warp
    // would trace rays to different lamps in the same step. In list order
    // the same slot of adjacent lanes usually names the same light, keeping
    // the warp's rays coherent (empty slots, 0xffffffff, sort last).
    {
        // Five-comparator sorting network.
        const uint a01 = min(bestEntry[0], bestEntry[1]), b01 = max(bestEntry[0], bestEntry[1]);
        const uint a23 = min(bestEntry[2], bestEntry[3]), b23 = max(bestEntry[2], bestEntry[3]);
        bestEntry[0] = min(a01, a23);
        bestEntry[3] = max(b01, b23);
        bestEntry[1] = min(max(a01, a23), min(b01, b23));
        bestEntry[2] = max(max(a01, a23), min(b01, b23));
    }
    const bool traceRays = !converged || reuseTurn;
    uint slots[MaxTracedLights];
    [unroll] for (uint k = 0u; k < MaxTracedLights; ++k)
    {
        slots[k] = 0u;
        const uint entry = bestEntry[k];
        if (entry == 0xffffffffu) continue;
        const uint lightIndex = clusterLightIndices[range.lightOffset + entry];
        const LocalLightData light = localLights[lightIndex];
        const float3 toLight = light.positionRange.xyz + sourceOffset(id, frame.temporalSampleIndex, entry) - origin;
        const float distanceToLight = length(toLight);
        RayDesc ray;
        ray.Origin = origin;
        ray.Direction = toLight / distanceToLight;
        ray.TMin = 0.001;
        // Stop short of the light: a lamp sits inside its own glass, cage and
        // base plate, all in the acceleration structure, up to ~0.5 m away.
        ray.TMax = max(distanceToLight - 0.6, 0.0);
        if (!traceRays)
            slots[k] = 0xa000u | (lightIndex & 0x3ffu);
        else
            slots[k] = 0x8000u | (occluded(ray) ? 0x4000u : 0u) | (lightIndex & 0x3ffu);
    }
    const uint2 samples = uint2(slots[0] | (slots[1] << 16u), slots[2] | (slots[3] << 16u));
    shadowMask[id.xy] = samples;
}

#if LOCAL_SHADOW_RAYGEN
// One launch per 2x2 quad.
[shader("raygeneration")]
void rayGen()
{
    traceQuad(DispatchRaysIndex().xy);
}

[shader("miss")]
void miss(inout ShadowPayload payload)
{
    payload.hit = 0u;
}

// Present only so the hit group exists; shadow rays skip closest-hit shading.
[shader("closesthit")]
void closestHit(inout ShadowPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    payload.hit = 1u;
}
#else
[numthreads(8, 8, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID)
{
    traceQuad(id.xy);
}
#endif
