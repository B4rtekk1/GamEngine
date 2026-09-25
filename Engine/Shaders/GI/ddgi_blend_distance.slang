import GI.ddgi_common;
[[vk::push_constant]] DDGIConstants ddgi;

[[vk::binding(5, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> rayData;
[[vk::binding(7, 1), vk::image_format("rg16f")]] RWTexture2D<float2> distanceAtlas;
[[vk::binding(9, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> probeData;
[[vk::binding(12, 1)]] StructuredBuffer<DDGIProbeUpdate> probeUpdateList;

groupshared uint rejectUpdate;

[numthreads(18, 18, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.z >= ddgiUpdateCount(ddgi)) return;
    const DDGIProbeUpdate update = probeUpdateList[id.z];
    const uint probeIndex = update.probeIndex;
    const uint2 atlasPixel = ddgiAtlasBase(probeIndex, DDGI_DISTANCE_INTERIOR) + id.xy;
    const float state = probeData[int2(ddgiProbePixel(probeIndex))].w;
    if (state < 0.5) {
        distanceAtlas[int2(atlasPixel)] = float2(0.0);
        return;
    }
    const uint rayCount = uint(ddgi.scrollOffsetRays.w);
    if (id.x == 0u && id.y == 0u) {
        uint backfaceCount = 0u;
        for (uint rayIndex = 0u; rayIndex < rayCount; ++rayIndex)
            backfaceCount += rayData[int2(rayIndex, probeIndex)].a < 0.0 ? 1u : 0u;
        rejectUpdate = float(backfaceCount) / float(rayCount) >
            DDGI_RANDOM_BACKFACE_THRESHOLD ? 1u : 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    if (rejectUpdate != 0u) return;

    const uint2 texel = id.xy;
    const uint last = DDGI_DISTANCE_INTERIOR + 1u;
    const bool border = texel.x == 0u || texel.y == 0u ||
                        texel.x == last || texel.y == last;
    if (!border) {
        const float2 oct = (float2(texel - 1u) + 0.5) /
                           float(DDGI_DISTANCE_INTERIOR) * 2.0 - 1.0;
        const float3 direction = ddgiOctDecode(oct);
        float mean = 0.0;
        float meanSquared = 0.0;
        float weightSum = 0.0;
        const float spacing = ddgi.originSpacing.w;
        const float visibilityMaxDistance = min(
            ddgi.distanceInitialized.x,
            1.5 * length(float3(spacing, spacing, spacing)));
        const float4 rotation = ddgiRayRotation(uint(ddgi.scrollDeltaFrame.w), rayCount);
        for (uint rayIndex = 0u; rayIndex < rayCount; ++rayIndex) {
            const float3 rayDirection = ddgiRayDirection(rayIndex, rayCount, rotation);
            const float weight = pow(max(dot(direction, rayDirection), 0.0), 50.0);
            const float distance = min(
                abs(rayData[int2(rayIndex, probeIndex)].a),
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
        distanceAtlas[int2(atlasPixel)] = blended;
    }

    AllMemoryBarrierWithGroupSync();
    if (border) {
        const uint2 source = ddgiAtlasBase(probeIndex, DDGI_DISTANCE_INTERIOR) +
                             ddgiBorderSource(texel, DDGI_DISTANCE_INTERIOR);
        distanceAtlas[int2(atlasPixel)] = distanceAtlas[int2(source)];
    }
}
