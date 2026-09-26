import GI.ddgi_common;
[[vk::push_constant]] DDGIConstants ddgi;

[[vk::binding(5, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> rayData;
[[vk::binding(6, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> irradianceAtlas;
[[vk::binding(9, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> probeData;
[[vk::binding(11, 1)]] RWStructuredBuffer<DDGIProbeState> probeStates;
[[vk::binding(12, 1)]] StructuredBuffer<DDGIProbeUpdate> probeUpdateList;
[[vk::binding(18, 1)]] StructuredBuffer<float4> rayDirections;

groupshared uint rejectUpdate;
// Each ray is loaded and its direction evaluated once per probe workgroup.
groupshared float4 cachedRays[DDGI_MAX_RAYS];
groupshared float3 cachedDirections[DDGI_MAX_RAYS];
groupshared float4 blendedTexels[36];
groupshared float variabilitySamples[36];

[numthreads(6, 6, 1)]
void main(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID, uint groupIndex : SV_GroupIndex) {
    if (group.z >= ddgiUpdateCount(ddgi)) return;
    const DDGIProbeUpdate update = probeUpdateList[group.z];
    const uint probeIndex = update.probeIndex;
    if (probeIndex >= DDGI_PROBES) return;
    const uint2 atlasBase = ddgiAtlasBase(probeIndex, DDGI_IRRADIANCE_INTERIOR);
    const uint2 atlasPixel = atlasBase + local.xy + 1u;
    const float state = probeData[int2(ddgiProbePixel(probeIndex))].w;
    if (state < 0.5) {
        for (uint pixel = groupIndex; pixel < 64u; pixel += 36u)
            irradianceAtlas[int2(atlasBase + uint2(pixel % 8u, pixel / 8u))] = float4(0.0);
        return;
    }
    const uint rayCount = ddgiRandomRayCount(ddgi, update.relocated);
    for (uint rayIndex = groupIndex; rayIndex < rayCount; rayIndex += 36u) {
        cachedRays[rayIndex] = rayData[int2(rayIndex, group.z)];
        const uint directionIndex = ddgiRandomDirectionIndex(
            rayIndex, rayCount, ddgiRandomRayCount(ddgi, 0u));
        cachedDirections[rayIndex] = rayDirections[directionIndex].xyz;
    }
    GroupMemoryBarrierWithGroupSync();
    if (groupIndex == 0u) {
        uint backfaceCount = 0u;
        const uint maxBackfaces = uint(float(rayCount) * DDGI_RANDOM_BACKFACE_THRESHOLD);
        for (uint rayIndex = 0u; rayIndex < rayCount; ++rayIndex)
            backfaceCount += cachedRays[rayIndex].a < 0.0 ? 1u : 0u;
        rejectUpdate = backfaceCount >= maxBackfaces ? 1u : 0u;
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
    // RTXGI stores half the cosine-weighted radiance mean. The 2*PI
    // hemisphere factor is applied after spatial filtering at lookup.
    const float3 encodedIrradiance = ddgiEncodeIrradiance(
        sum / max(weightSum, float(rayCount) * 1e-9));
    float3 blended = encodedIrradiance;
    float texelVariability = 0.0;
    // The first update has no valid history. History is weighted per probe update.
    if (ddgi.distanceInitialized.y > 0.5 && state < 1.5) {
        const float3 previous = irradianceAtlas[int2(atlasPixel)].rgb;
        float3 delta = encodedIrradiance - previous;
        const float3 luminanceWeights = float3(0.2126, 0.7152, 0.0722);
        float hysteresis = dot(previous, previous) == 0.0 ? 0.0 : DDGI_HYSTERESIS;
        const float3 darkening = previous - encodedIrradiance;
        if (max(max(darkening.x, darkening.y), darkening.z) >
            DDGI_IRRADIANCE_CHANGE_THRESHOLD)
            hysteresis = max(0.0, hysteresis - 0.75);
        if (dot(delta, luminanceWeights) > DDGI_BRIGHTNESS_THRESHOLD)
            delta *= 0.25;
        float3 lerpDelta = (1.0 - hysteresis) * delta;
        if (max(encodedIrradiance.x, max(encodedIrradiance.y, encodedIrradiance.z)) <
            max(previous.x, max(previous.y, previous.z))) {
            lerpDelta = min(max(float3(1.0 / 1024.0), abs(lerpDelta)), abs(delta)) *
                        sign(lerpDelta);
        }
        blended = previous + lerpDelta;
        const float3 variance = (encodedIrradiance - previous) *
                                (encodedIrradiance - blended);
        const float meanLuminance = dot(blended, luminanceWeights);
        texelVariability = meanLuminance <= 1.0 / 1024.0 ? 0.0 :
            sqrt(max(dot(variance, luminanceWeights), 0.0)) / meanLuminance;
    }
    variabilitySamples[groupIndex] = texelVariability;
    blendedTexels[groupIndex] = float4(blended, 1.0);
    irradianceAtlas[int2(atlasPixel)] = float4(blended, 1.0);

    // Borders copy the same value as the interior, without reading the UAV.
    GroupMemoryBarrierWithGroupSync();
    if (groupIndex == 0u) {
        float variability = 0.0;
        for (uint texel = 0u; texel < 36u; ++texel)
            variability += variabilitySamples[texel];
        DDGIProbeState probeState = probeStates[probeIndex];
        probeState.variability = lerp(probeState.variability, variability / 36.0, 0.2);
        probeState.state = DDGI_ACCEPTED_UPDATE_STATE;
        probeStates[probeIndex] = probeState;
    }
    // Enumerate top/bottom rows, then the sides without their corners.
    for (uint edge = groupIndex; edge < 28u; edge += 36u) {
        uint2 texel;
        if (edge < 16u)
            texel = uint2(edge % 8u, edge < 8u ? 0u : 7u);
        else {
            const uint side = edge - 16u;
            texel = uint2(side < 6u ? 0u : 7u, side % 6u + 1u);
        }
        const uint2 source = ddgiBorderSource(texel, DDGI_IRRADIANCE_INTERIOR) - 1u;
        irradianceAtlas[int2(atlasBase + texel)] = blendedTexels[source.y * 6u + source.x];
    }
}
