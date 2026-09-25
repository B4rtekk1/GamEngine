import GI.ddgi_common;
[[vk::push_constant]] DDGIConstants ddgi;
[[vk::binding(9, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> probeData;
[[vk::binding(11, 1)]] RWStructuredBuffer<DDGIProbeState> probeStates;
[[vk::binding(12, 1)]] StructuredBuffer<DDGIProbeUpdate> probeUpdateList;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= ddgiUpdateCount(ddgi)) return;
    const DDGIProbeUpdate update = probeUpdateList[id.x];
    const uint probeIndex = update.probeIndex;
    if (probeIndex >= DDGI_PROBES) return;
    const int2 pixel = int2(ddgiProbePixel(probeIndex));
    const float4 probe = probeData[pixel];
    DDGIProbeState state = probeStates[probeIndex];
    const bool accepted = probe.w > 0.5 && state.state == DDGI_ACCEPTED_UPDATE_STATE;
    if (accepted)
        probeData[pixel] = float4(probe.xyz,
            probe.w > 1.5 ? DDGI_REACTIVATION_STATE : probe.w);
    // Scheduling tracks attempts separately from atlas validity. Retrying
    // rejected probes at startup priority would starve the rest of the grid.
    if (probe.w < 0.5)
        state.state = 0u;
    else if (probe.w > 1.5 && !accepted)
        state.state = 2u;
    else
        state.state = 1u;
    state.updateCount = min(state.updateCount + 1u, DDGI_MIN_CONVERGENCE_UPDATES);
    if (probe.w < 0.5) state.variability = 0.0;
    if (probe.w < 0.5 || (accepted && probe.w < 1.5 &&
                          state.variability < DDGI_VARIABILITY_THRESHOLD))
        state.stableUpdates = min(state.stableUpdates + 1u, DDGI_STABLE_UPDATES);
    else
        state.stableUpdates = 0u;
    probeStates[probeIndex] = state;
}
