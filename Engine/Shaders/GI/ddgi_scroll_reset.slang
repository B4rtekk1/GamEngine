import GI.ddgi_common;
[[vk::push_constant]] DDGIConstants ddgi;

[[vk::binding(6, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> irradianceAtlas;
[[vk::binding(7, 1), vk::image_format("rg16f")]] RWTexture2D<float2> distanceAtlas;
[[vk::binding(9, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> probeData;
[[vk::binding(11, 1)]] RWStructuredBuffer<DDGIProbeState> probeStates;

[[vk::binding(16, 1)]] StructuredBuffer<uint> regionCache;
[[vk::binding(17, 1)]] StructuredBuffer<uint> cacheMapping;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= DDGI_PROBES) return;
    const uint3 logical = ddgiLogicalGrid(id.x, ddgi.scrollOffsetRays.xyz);
    const int3 oldLogical = int3(logical) + ddgi.scrollDeltaFrame.xyz;
    const bool exposed = ddgi.distanceInitialized.y < 0.5 ||
        any(oldLogical < 0) || any(oldLogical >= int3(DDGI_COUNTS));
    if (!exposed) return;
    const uint base = cacheMapping[DDGI_PROBES + id.x];
    if (regionCache[base] != 0u) {
        DDGIProbeState cached;
        // Cached lighting is a starting history, not a validated probe.
        cached.state = 2u;
        cached.lastUpdatedFrame = regionCache[base + 2u];
        cached.variability = asfloat(regionCache[base + 3u]);
        cached.updateCount = regionCache[base + 4u];
        // Revisit cached lighting after the local recursive-gather neighborhood
        // changed, while retaining its atlas and accumulated convergence count.
        cached.stableUpdates = 0u;
        probeStates[id.x] = cached;
        float4 cachedProbe = float4(
            ddgiUnpackHalf2(regionCache[base + 6u]), ddgiUnpackHalf2(regionCache[base + 7u]));
        cachedProbe.w = 2.0;
        probeData[int2(ddgiProbePixel(id.x))] = cachedProbe;
        const uint2 ib = ddgiAtlasBase(id.x, DDGI_IRRADIANCE_INTERIOR);
        for (uint i = 0u; i < 100u; ++i)
            irradianceAtlas[int2(ib + uint2(i % 10u, i / 10u))] = float4(
                ddgiUnpackHalf2(regionCache[base + 8u + i * 2u]),
                ddgiUnpackHalf2(regionCache[base + 9u + i * 2u]));
        const uint2 db = ddgiAtlasBase(id.x, DDGI_DISTANCE_INTERIOR);
        for (uint i = 0u; i < 324u; ++i)
            distanceAtlas[int2(db + uint2(i % 18u, i / 18u))] =
                ddgiUnpackHalf2(regionCache[base + 208u + i]);
        return;
    }
    probeData[int2(ddgiProbePixel(id.x))] = float4(0.0, 0.0, 0.0, 2.0);
    DDGIProbeState state;
    state.state = 2u;
    state.lastUpdatedFrame = 0u;
    state.variability = 0.0;
    state.updateCount = 0u;
    state.stableUpdates = 0u;
    probeStates[id.x] = state;
    const uint2 irradianceBase = ddgiAtlasBase(id.x, DDGI_IRRADIANCE_INTERIOR);
    for (uint y = 0u; y < DDGI_IRRADIANCE_INTERIOR + 2u; ++y)
        for (uint x = 0u; x < DDGI_IRRADIANCE_INTERIOR + 2u; ++x)
            irradianceAtlas[int2(irradianceBase + uint2(x, y))] = float4(0.0);
    const uint2 distanceBase = ddgiAtlasBase(id.x, DDGI_DISTANCE_INTERIOR);
    for (uint y = 0u; y < DDGI_DISTANCE_INTERIOR + 2u; ++y)
        for (uint x = 0u; x < DDGI_DISTANCE_INTERIOR + 2u; ++x)
            distanceAtlas[int2(distanceBase + uint2(x, y))] = float2(0.0);
}
