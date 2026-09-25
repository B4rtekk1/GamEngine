import GI.ddgi_common;
[[vk::binding(6, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> irradianceAtlas;
[[vk::binding(7, 1), vk::image_format("rg16f")]] RWTexture2D<float2> distanceAtlas;
[[vk::binding(9, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> probeData;
[[vk::binding(11, 1)]] StructuredBuffer<DDGIProbeState> probeStates;
[[vk::binding(16, 1)]] RWStructuredBuffer<uint> regionCache;
[[vk::binding(17, 1)]] StructuredBuffer<uint> cacheMapping;

// One group owns a complete probe record. A dispatch barrier publishes it
// before restoration; each physical probe maps to a unique world cell.
[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
    const uint probe = group.x;
    const uint base = cacheMapping[probe];
    if (lane == 0u) {
        const DDGIProbeState state = probeStates[probe];
        const float4 data = probeData[int2(ddgiProbePixel(probe))];
        regionCache[base] = 1u;
        regionCache[base + 1u] = state.state;
        regionCache[base + 2u] = state.lastUpdatedFrame;
        regionCache[base + 3u] = asuint(state.variability);
        regionCache[base + 4u] = state.updateCount;
        regionCache[base + 5u] = state.stableUpdates;
        regionCache[base + 6u] = ddgiPackHalf2(data.xy);
        regionCache[base + 7u] = ddgiPackHalf2(data.zw);
    }
    const uint2 irradianceBase = ddgiAtlasBase(probe, DDGI_IRRADIANCE_INTERIOR);
    for (uint i = lane; i < 100u; i += 64u) {
        const float4 value = irradianceAtlas[int2(irradianceBase + uint2(i % 10u, i / 10u))];
        regionCache[base + 8u + i * 2u] = ddgiPackHalf2(value.xy);
        regionCache[base + 9u + i * 2u] = ddgiPackHalf2(value.zw);
    }
    const uint2 distanceBase = ddgiAtlasBase(probe, DDGI_DISTANCE_INTERIOR);
    for (uint i = lane; i < 324u; i += 64u)
        regionCache[base + 208u + i] = ddgiPackHalf2(
            distanceAtlas[int2(distanceBase + uint2(i % 18u, i / 18u))]);
}
