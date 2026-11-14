// Denoiser for the ray-traced local light shadows. The trace writes, per 2x2
// quad, at most four lights with one binary visibility sample each, aimed at a
// jittered point of the emitter. This kernel turns those samples into soft,
// stable per-light occlusion, at the same quad resolution (forward shading
// upsamples it by depth):
//
//  1. Spatial: a 5x5 cross-bilateral gather (depth, normal) averages the
//     samples of the lights traced at the quad over its neighbours.
//  2. Temporal: the result is blended with last frame's, reprojected by
//     camera motion and rejected where the stored depth disagrees.
//
// Lights are keyed by their global index, so the result does not depend on
// which cluster list a pixel walked; forward shading looks them up by index.
import Common.SceneData;

// Trace output, one texel per quad. Four 16-bit slots in x (0, 1) and y (2, 3):
// bits 0-9 light index, bit 13 not traced this frame, bit 14 occluded, bit 15
// slot valid.
[[vk::binding(0, 0), vk::image_format("rg32ui")]] RWTexture2D<uint2> rawMask;
[[vk::binding(1, 0)]] Sampler2D depthTexture;
[[vk::binding(2, 0)]] Sampler2D viewNormalTexture;
// Last frame's result (see output).
[[vk::binding(3, 0)]] Sampler2D<uint4> history;
// x: light indices 0 | 1 << 16, y: 2 | 3 << 16, z: four 8-bit occlusions
// (0: lit), w: view depth of the traced pixel with its low 8 bits replaced by
// the history length.
[[vk::binding(4, 0), vk::image_format("rgba32ui")]] RWTexture2D<uint4> output;
[[vk::binding(5, 0)]] ConstantBuffer<RendererFrameData> frame;

static const int Radius = 2;
static const int GroupSize = 8;
static const int TileSize = GroupSize + 2 * Radius;
static const uint OutputSlots = 4u;
// Shortest effective history: about ten frames of accumulation.
static const float MinTemporalWeight = 0.1;
// Spatial weight of a full 5x5 neighbourhood of fresh samples on a typical
// surface (the Gaussian alone sums to ~11.7). Frames with fewer fresh samples
// (history reuse) blend in proportionally less, so the effective sample
// count, and with it the noise level, stays the same.
static const float FullSpatialWeight = 8.0;

groupshared uint2 tileRaw[TileSize * TileSize];
groupshared float tileDepth[TileSize * TileSize];
groupshared float3 tileNormal[TileSize * TileSize];

float3 decodeOct(float2 e)
{
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * select(n.xy >= 0.0, 1.0, -1.0);
    return normalize(n);
}

// The pixel each quad traced this frame (as local_light_shadow.slang).
static const int2 QuadOrder[4] = { int2(0, 0), int2(1, 1), int2(1, 0), int2(0, 1) };

float3 reconstructViewPosition(int2 pixel, float depth, int2 extent)
{
    const float2 uv = (float2(pixel) + 0.5) / float2(extent);
    const float4 position = mul(float4(uv * 2.0 - 1.0, depth, 1.0), frame.inverseProjection);
    return position.xyz / position.w;
}

uint rawSlot(uint2 raw, uint slot)
{
    return ((slot < 2u ? raw.x : raw.y) >> ((slot & 1u) * 16u)) & 0xffffu;
}

uint outputLight(uint4 value, uint slot)
{
    return ((slot < 2u ? value.x : value.y) >> ((slot & 1u) * 16u)) & 0xffffu;
}

float outputOcclusion(uint4 value, uint slot)
{
    return float((value.z >> (slot * 8u)) & 0xffu) / 255.0;
}

[numthreads(GroupSize, GroupSize, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID, uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    uint quadWidth, quadHeight, width, height;
    output.GetDimensions(quadWidth, quadHeight);
    depthTexture.GetDimensions(width, height);
    const int2 quads = int2(quadWidth, quadHeight);
    const int2 extent = int2(width, height);
    const int2 traced = QuadOrder[frame.temporalSampleIndex & 3u];

    // Stage the group's quads plus the filter apron.
    const int2 tileOrigin = int2(groupId.xy) * GroupSize - Radius;
    for (uint t = groupIndex; t < uint(TileSize * TileSize); t += uint(GroupSize * GroupSize))
    {
        const int2 quad = tileOrigin + int2(int(t) % TileSize, int(t) / TileSize);
        uint2 raw = uint2(0u, 0u);
        float viewDepth = 0.0;
        float3 normal = float3(0.0, 0.0, 1.0);
        if (all(quad >= 0) && all(quad < quads))
        {
            const int2 pixel = min(quad * 2 + traced, extent - 1);
            const float depth = depthTexture.Load(int3(pixel, 0)).x;
            if (depth < 0.999999)
            {
                raw = rawMask[quad];
                viewDepth = -reconstructViewPosition(pixel, depth, extent).z;
                normal = decodeOct(viewNormalTexture.Load(int3(pixel, 0)).xy);
            }
        }
        tileRaw[t] = raw;
        tileDepth[t] = viewDepth;
        tileNormal[t] = normal;
    }
    GroupMemoryBarrierWithGroupSync();

    const int2 quad = int2(id.xy);
    if (any(quad >= quads)) return;
    const int2 local = quad - int2(groupId.xy) * GroupSize + Radius;
    const uint center = uint(local.y * TileSize + local.x);
    const float centerDepth = tileDepth[center];
    if (centerDepth <= 0.0)
    {
        output[quad] = uint4(0u, 0u, 0u, 0u);
        return;
    }
    const float3 centerNormal = tileNormal[center];

    // Reproject last frame's result by camera motion.
    const int2 pixel = min(quad * 2 + traced, extent - 1);
    const float depth = depthTexture.Load(int3(pixel, 0)).x;
    const float3 viewPosition = reconstructViewPosition(pixel, depth, extent);
    const float3 worldPosition = mul(float4(viewPosition, 1.0), frame.inverseView).xyz;
    const float4 previousView = mul(float4(worldPosition, 1.0), frame.previousView);
    const float4 previousClip = mul(previousView, frame.previousProjection);
    uint4 previous = uint4(0u, 0u, 0u, 0u);
    uint historyLength = 0u;
    if (previousClip.w > 0.0)
    {
        const float2 previousUv = previousClip.xy / previousClip.w * 0.5 + 0.5;
        const int2 previousQuad = int2(floor(previousUv * float2(extent))) / 2;
        if (all(previousUv >= 0.0) && all(previousQuad < quads))
        {
            previous = history.Load(int3(previousQuad, 0));
            const float previousDepth = asfloat(previous.w & ~0xffu);
            const float expectedDepth = -previousView.z / previousView.w;
            historyLength = previous.w & 0xffu;
            // The quad traced another of its pixels then: allow for the slope.
            if (abs(previousDepth - expectedDepth) > 0.05 * expectedDepth + 0.03)
                historyLength = 0u;
        }
    }

    // The lights to filter: those traced at the most similar nearby texel
    // (the pixel itself unless it is a silhouette its quad did not share).
    const float depthTolerance = 0.03 * centerDepth + 0.01;
    float weights[(2 * Radius + 1) * (2 * Radius + 1)];
    uint2 anchor = uint2(0u, 0u);
    float anchorWeight = 0.0;
    [unroll] for (int dy = -Radius; dy <= Radius; ++dy)
    {
        [unroll] for (int dx = -Radius; dx <= Radius; ++dx)
        {
            const uint t = uint((local.y + dy) * TileSize + local.x + dx);
            const float depthWeight = saturate(1.0 - abs(tileDepth[t] - centerDepth) / depthTolerance);
            float normalWeight = saturate(dot(tileNormal[t], centerNormal));
            normalWeight *= normalWeight;
            normalWeight *= normalWeight;
            normalWeight *= normalWeight;
            normalWeight *= normalWeight;
            const float spatialWeight = exp(-float(dx * dx + dy * dy) / (2.0 * 1.5 * 1.5));
            const uint2 raw = tileRaw[t];
            const float weight = (raw.x | raw.y) != 0u ? depthWeight * normalWeight * spatialWeight : 0.0;
            weights[(dy + Radius) * (2 * Radius + 1) + dx + Radius] = weight;
            if (weight > anchorWeight)
            {
                anchorWeight = weight;
                anchor = raw;
            }
        }
    }
    // Which pixel of a quad traces rotates, and lights ranked near the cut-off
    // differ between them; replacing the set each frame would restart those
    // lights from a single sample. Keep last frame's lights and add the
    // anchor's: into empty slots first, else over lights the anchor lacks.
    // 0xffff marks an empty slot and never matches a (10-bit) light index.
    uint4 anchorIds;
    [unroll] for (uint s = 0u; s < 4u; ++s)
    {
        const uint slot = rawSlot(anchor, s);
        anchorIds[s] = (slot & 0x8000u) != 0u ? (slot & 0x3ffu) : 0xffffu;
    }
    uint4 lightIds = anchorIds;
    if (historyLength != 0u)
    {
        [unroll] for (uint s = 0u; s < 4u; ++s)
            lightIds[s] = outputLight(previous, s);
        // Branch-free (selects, no dynamic indexing, which would spill the
        // vectors to local memory).
        [unroll] for (uint a = 0u; a < 4u; ++a)
        {
            const uint light = anchorIds[a];
            const bool insert = light != 0xffffu && !any(lightIds == light);
            const bool4 empty = lightIds == 0xffffu;
            const bool4 stale = !(lightIds == anchorIds.x || lightIds == anchorIds.y || lightIds == anchorIds.z ||
                                  lightIds == anchorIds.w);
            const bool4 candidate = any(empty) ? empty : stale;
            // First candidate slot only.
            const bool4 first = candidate && bool4(true, !candidate.x, !candidate.x && !candidate.y,
                                                   !candidate.x && !candidate.y && !candidate.z);
            lightIds = select(insert && first, uint4(light), lightIds);
        }
    }

    // Spatial estimate of each: weighted mean of its samples around here.
    float4 occlusionSum = 0.0;
    float4 weightSum = 0.0;
    [unroll] for (int dy = -Radius; dy <= Radius; ++dy)
    {
        [unroll] for (int dx = -Radius; dx <= Radius; ++dx)
        {
            const float weight = weights[(dy + Radius) * (2 * Radius + 1) + dx + Radius];
            if (weight <= 1e-3) continue;
            const uint2 raw = tileRaw[uint((local.y + dy) * TileSize + local.x + dx)];
            [unroll] for (uint s = 0u; s < 4u; ++s)
            {
                const uint slot = rawSlot(raw, s);
                // An invalid slot yields 0xffff and matches nothing.
                // Untraced slots name a light but carry no sample.
                const uint light = (slot & 0xa000u) == 0x8000u ? (slot & 0x3ffu) : 0xfffeu;
                const float4 match = select(lightIds == light, float4(weight), float4(0.0));
                weightSum += match;
                occlusionSum += (slot & 0x4000u) != 0u ? match : float4(0.0);
            }
        }
    }

    const float temporalWeight = max(1.0 / float(historyLength + 1u), MinTemporalWeight);
    float4 occlusion = occlusionSum / max(weightSum, 1e-4);
    bool4 inHistory = false;
    if (historyLength != 0u)
    {
        [unroll] for (uint s = 0u; s < OutputSlots; ++s)
        {
            const float4 previousOcclusion = outputOcclusion(previous, s);
            const bool4 match = lightIds == outputLight(previous, s);
            inHistory = inHistory || match;
            // Without fresh samples around (history reuse), keep the history.
            const float4 weight = temporalWeight * saturate(weightSum / FullSpatialWeight);
            const float4 blended = lerp(previousOcclusion, occlusion, weight);
            occlusion = select(match, blended, occlusion);
        }
    }
    // A light with neither history nor a fresh sample has no estimate yet:
    // leave its slot empty rather than start it as lit, so the trace sees it
    // missing from the history and samples this quad at full rate.
    const bool4 unknown = !inHistory && weightSum <= 1e-3;
    lightIds = select(unknown, uint4(0xffffu), lightIds);
    occlusion = select(unknown, float4(0.0), occlusion);
    // Dithered rounding: with small blend weights a fixed rounding would
    // stall the accumulation once a frame's change is under half a step.
    const uint hash = (uint(quad.x) * 1973u + uint(quad.y) * 9277u + frame.temporalSampleIndex * 26699u) | 1u;
    const float4 dither = frac(float4(hash * uint4(0x9e3779b9u, 0x85ebca6bu, 0xc2b2ae35u, 0x27d4eb2fu) >> 8u) /
                               16777216.0);
    const uint4 quantized = uint4(min(saturate(occlusion) * 255.0 + dither, 255.0));
    uint4 result;
    result.x = lightIds.x | (lightIds.y << 16u);
    result.y = lightIds.z | (lightIds.w << 16u);
    result.z = quantized.x | (quantized.y << 8u) | (quantized.z << 16u) | (quantized.w << 24u);
    result.w = (asuint(centerDepth) & ~0xffu) | min(historyLength + 1u, 255u);
    output[quad] = result;
}
