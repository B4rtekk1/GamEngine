import GI.ddgi_common;
[[vk::push_constant]] DDGIConstants ddgi;

[[vk::binding(5, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> rayData;
[[vk::binding(7, 1), vk::image_format("rg16f")]] RWTexture2D<float2> distanceAtlas;
[[vk::binding(9, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> probeData;
[[vk::binding(12, 1)]] StructuredBuffer<DDGIProbeUpdate> probeUpdateList;

groupshared uint rejectUpdate;
// Each ray is loaded and its direction evaluated once per probe workgroup.
// xyz stores direction; w stores the signed distance, with no unused RGB.
groupshared float4 cachedRays[DDGI_MAX_RAYS];
groupshared float2 blendedTexels[256];

[numthreads(16, 16, 1)]
void main(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID, uint groupIndex : SV_GroupIndex) {
    if (group.z >= ddgiUpdateCount(ddgi)) return;
    const DDGIProbeUpdate update = probeUpdateList[group.z];
    const uint probeIndex = update.probeIndex;
    const uint2 atlasBase = ddgiAtlasBase(probeIndex, DDGI_DISTANCE_INTERIOR);
    const uint2 atlasPixel = atlasBase + local.xy + 1u;
    const float state = probeData[int2(ddgiProbePixel(probeIndex))].w;
    if (state < 0.5) {
        for (uint pixel = groupIndex; pixel < 324u; pixel += 256u)
            distanceAtlas[int2(atlasBase + uint2(pixel % 18u, pixel / 18u))] = float2(0.0);
        return;
    }
    const uint rayCount = uint(ddgi.scrollOffsetRays.w);
    const float4 rotation = ddgiRayRotation(uint(ddgi.scrollDeltaFrame.w), rayCount);
    for (uint rayIndex = groupIndex; rayIndex < rayCount; rayIndex += 256u) {
        cachedRays[rayIndex] = float4(ddgiRayDirection(rayIndex, rayCount, rotation),
                                     rayData[int2(rayIndex, group.z)].a);
    }
    GroupMemoryBarrierWithGroupSync();
    if (groupIndex == 0u) {
        uint backfaceCount = 0u;
        for (uint rayIndex = 0u; rayIndex < rayCount; ++rayIndex)
            backfaceCount += cachedRays[rayIndex].a < 0.0 ? 1u : 0u;
        rejectUpdate = float(backfaceCount) / float(rayCount) >
            DDGI_RANDOM_BACKFACE_THRESHOLD ? 1u : 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    if (rejectUpdate != 0u) return;

    const float2 oct = (float2(local.xy) + 0.5) /
                       float(DDGI_DISTANCE_INTERIOR) * 2.0 - 1.0;
    const float3 direction = ddgiOctDecode(oct);
    float mean = 0.0;
    float meanSquared = 0.0;
    float weightSum = 0.0;
    const float spacing = ddgi.originSpacing.w;
    const float visibilityMaxDistance = min(
        ddgi.distanceInitialized.x,
        1.5 * length(float3(spacing, spacing, spacing)));
    for (uint rayIndex = 0u; rayIndex < rayCount; ++rayIndex) {
        const float3 rayDirection = cachedRays[rayIndex].xyz;
        const float weight = pow(max(dot(direction, rayDirection), 0.0), 50.0);
        const float distance = min(
            abs(cachedRays[rayIndex].a),
            visibilityMaxDistance);
        mean += distance * weight;
        meanSquared += distance * distance * weight;
        weightSum += weight;
    }
    const float2 result = weightSum > 0.0
        ? float2(mean, meanSquared) / weightSum
        : float2(visibilityMaxDistance, visibilityMaxDistance * visibilityMaxDistance);
    float2 blended = result;
    if (ddgi.distanceInitialized.y > 0.5 && state < 1.5) {
        const float hysteresis = pow(0.97, float(update.framesSinceLastUpdate));
        blended = lerp(result, distanceAtlas[int2(atlasPixel)], hysteresis);
    }
    blendedTexels[groupIndex] = blended;
    distanceAtlas[int2(atlasPixel)] = blended;

    // Borders copy the same value as the interior, without reading the UAV.
    GroupMemoryBarrierWithGroupSync();
    // Enumerate top/bottom rows, then the sides without their corners.
    for (uint edge = groupIndex; edge < 68u; edge += 256u) {
        uint2 texel;
        if (edge < 36u)
            texel = uint2(edge % 18u, edge < 18u ? 0u : 17u);
        else {
            const uint side = edge - 36u;
            texel = uint2(side < 16u ? 0u : 17u, side % 16u + 1u);
        }
        const uint2 source = ddgiBorderSource(texel, DDGI_DISTANCE_INTERIOR) - 1u;
        distanceAtlas[int2(atlasBase + texel)] = blendedTexels[source.y * 16u + source.x];
    }
}
