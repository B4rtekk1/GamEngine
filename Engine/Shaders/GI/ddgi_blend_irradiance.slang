import GI.ddgi_common;
[[vk::push_constant]] DDGIConstants ddgi;

[[vk::binding(5, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> rayData;
[[vk::binding(6, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> irradianceAtlas;
[[vk::binding(9, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> probeData;
[[vk::binding(11, 1)]] RWStructuredBuffer<DDGIProbeState> probeStates;
[[vk::binding(12, 1)]] StructuredBuffer<DDGIProbeUpdate> probeUpdateList;

groupshared uint rejectUpdate;
// Each ray is loaded and its direction evaluated once per probe workgroup.
groupshared float4 cachedRays[DDGI_MAX_RAYS];
groupshared float3 cachedDirections[DDGI_MAX_RAYS];
groupshared float4 blendedTexels[64];
groupshared float variabilitySamples[64];

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID, uint groupIndex : SV_GroupIndex) {
    if (group.z >= ddgiUpdateCount(ddgi)) return;
    const DDGIProbeUpdate update = probeUpdateList[group.z];
    const uint probeIndex = update.probeIndex;
    if (probeIndex >= DDGI_PROBES) return;
    const uint2 atlasBase = ddgiAtlasBase(probeIndex, DDGI_IRRADIANCE_INTERIOR);
    const uint2 atlasPixel = atlasBase + local.xy + 1u;
    const float state = probeData[int2(ddgiProbePixel(probeIndex))].w;
    if (state < 0.5) {
        for (uint pixel = groupIndex; pixel < 100u; pixel += 64u)
            irradianceAtlas[int2(atlasBase + uint2(pixel % 10u, pixel / 10u))] = float4(0.0);
        return;
    }
    const uint rayCount = uint(ddgi.scrollOffsetRays.w);
    const float4 rotation = ddgiRayRotation(uint(ddgi.scrollDeltaFrame.w), rayCount);
    for (uint rayIndex = groupIndex; rayIndex < rayCount; rayIndex += 64u) {
        cachedRays[rayIndex] = rayData[int2(rayIndex, group.z)];
        cachedDirections[rayIndex] = ddgiRayDirection(rayIndex, rayCount, rotation);
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
                       float(DDGI_IRRADIANCE_INTERIOR) * 2.0 - 1.0;
    const float3 direction = ddgiOctDecode(oct);
    float3 sum = 0.0;
    float weightSum = 0.0;
    for (uint rayIndex = 0u; rayIndex < rayCount; ++rayIndex) {
        const float4 sample = cachedRays[rayIndex];
        if (sample.a < 0.0) continue;
        const float3 rayDirection = cachedDirections[rayIndex];
        const float weight = max(dot(direction, rayDirection), 0.0);
        sum += sample.rgb * weight;
        weightSum += weight;
    }
    // The weighted mean is radiance. Its cosine-weighted hemisphere integral
    // is irradiance, so the atlas stores E for the forward Lambert term E / PI.
    const float3 irradiance = 3.14159265 * sum / max(weightSum, 1e-5);
    float3 blended = irradiance;
    float texelVariability = 1.0;
    // The first update has no valid history. History is weighted per probe update.
    if (ddgi.distanceInitialized.y > 0.5 && state < 1.5 &&
        irradianceAtlas[int2(atlasPixel)].a > 0.0) {
        const float3 previous = irradianceAtlas[int2(atlasPixel)].rgb;
        float3 delta = irradiance - previous;
        const float3 luminanceWeights = float3(0.2126, 0.7152, 0.0722);
        const float previousLuminance = dot(previous, luminanceWeights);
        const float hysteresis = 0.97;
        // Bound a single bright estimate before it can enter recursive probe
        // lighting on later frames. Repeated real changes still accumulate.
        const float increase = dot(delta, luminanceWeights);
        const float maxIncrease = max(0.10, previousLuminance);
        if (increase > maxIncrease)
            delta *= maxIncrease / increase;
        blended = previous + (1.0 - hysteresis) * delta;
        // Measure the new ray estimate before hysteresis, which can hide
        // persistent sampling noise while the atlas is still changing.
        texelVariability = abs(dot(irradiance - previous, luminanceWeights)) /
                           max(previousLuminance, 0.10);
    }
    variabilitySamples[groupIndex] = texelVariability;
    blendedTexels[groupIndex] = float4(blended, 1.0);
    irradianceAtlas[int2(atlasPixel)] = float4(blended, 1.0);

    // Borders copy the same value as the interior, without reading the UAV.
    GroupMemoryBarrierWithGroupSync();
    if (groupIndex == 0u) {
        float variability = 0.0;
        for (uint texel = 0u; texel < 64u; ++texel)
            variability += variabilitySamples[texel];
        DDGIProbeState probeState = probeStates[probeIndex];
        probeState.variability = lerp(probeState.variability, variability / 64.0, 0.2);
        probeStates[probeIndex] = probeState;
    }
    // Enumerate top/bottom rows, then the sides without their corners.
    for (uint edge = groupIndex; edge < 36u; edge += 64u) {
        uint2 texel;
        if (edge < 20u)
            texel = uint2(edge % 10u, edge < 10u ? 0u : 9u);
        else {
            const uint side = edge - 20u;
            texel = uint2(side < 8u ? 0u : 9u, side % 8u + 1u);
        }
        const uint2 source = ddgiBorderSource(texel, DDGI_IRRADIANCE_INTERIOR) - 1u;
        irradianceAtlas[int2(atlasBase + texel)] = blendedTexels[source.y * 8u + source.x];
    }
}
