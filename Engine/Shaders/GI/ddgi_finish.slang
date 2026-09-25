import GI.ddgi_common;
[[vk::push_constant]] DDGIConstants ddgi;
[[vk::binding(5, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> rayData;
[[vk::binding(9, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> probeData;
[[vk::binding(11, 1)]] RWStructuredBuffer<DDGIProbeState> probeStates;
[[vk::binding(12, 1)]] StructuredBuffer<DDGIProbeUpdate> probeUpdateList;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= ddgiUpdateCount(ddgi)) return;
    const uint probeIndex = probeUpdateList[id.x].probeIndex;
    const int2 pixel = int2(ddgiProbePixel(probeIndex));
    const float4 probe = probeData[pixel];
    if (probe.w > 0.5) {
        const uint rayCount = uint(ddgi.scrollOffsetRays.w);
        uint backfaces = 0u;
        for (uint rayIndex = 0u; rayIndex < rayCount; ++rayIndex)
            backfaces += rayData[int2(rayIndex, id.x)].a < 0.0 ? 1u : 0u;
        // Match both blend passes. A rejected update must not turn an empty
        // or relocated probe into valid history for the next frame.
        if (float(backfaces) / float(rayCount) <= DDGI_RANDOM_BACKFACE_THRESHOLD)
            probeData[pixel] = float4(probe.xyz, 1.0);
    }
    DDGIProbeState state = probeStates[probeIndex];
    // Scheduling tracks attempts separately from atlas validity. Retrying
    // rejected probes at startup priority would starve the rest of the grid.
    state.state = probe.w < 0.5 ? 0u : 1u;
    state.variability = 0.0;
    probeStates[probeIndex] = state;
}
