import GI.ddgi_common;
[[vk::push_constant]] DDGIConstants ddgi;
[[vk::binding(11, 1)]] RWStructuredBuffer<DDGIProbeState> probeStates;
[[vk::binding(12, 1)]] RWStructuredBuffer<DDGIProbeUpdate> probeUpdateList;

groupshared uint scores[DDGI_PROBES];
groupshared uint indices[DDGI_PROBES];

[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex) {
    for (uint i = groupIndex; i < DDGI_PROBES; i += 256u) {
        const DDGIProbeState state = probeStates[i];
        const uint age = min(uint(ddgi.scrollDeltaFrame.w) - state.lastUpdatedFrame, 4095u);
        // Newly exposed probes first, then the least recently updated probes.
        scores[i] = (uint(state.state == 2u) << 23u) | (age << 11u) | (2047u - i);
        indices[i] = i;
    }
    GroupMemoryBarrierWithGroupSync();
    // Sort 2048 probe keys in shared memory; only the cascade's quota is traced.
    for (uint width = 2u; width <= DDGI_PROBES; width <<= 1u) {
        for (uint stride = width >> 1u; stride > 0u; stride >>= 1u) {
            for (uint i = groupIndex; i < DDGI_PROBES; i += 256u) {
                const uint peer = i ^ stride;
                if (peer <= i) continue;
                const bool descending = (i & width) == 0u;
                if ((scores[i] < scores[peer]) == descending) {
                    const uint score = scores[i]; scores[i] = scores[peer]; scores[peer] = score;
                    const uint index = indices[i]; indices[i] = indices[peer]; indices[peer] = index;
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }
    if (groupIndex < ddgiUpdateCount(ddgi)) {
        const uint probeIndex = indices[groupIndex];
        const uint currentFrame = uint(ddgi.scrollDeltaFrame.w);
        DDGIProbeUpdate update;
        update.probeIndex = probeIndex;
        update.framesSinceLastUpdate = max(
            currentFrame - probeStates[probeIndex].lastUpdatedFrame, 1u);
        probeUpdateList[groupIndex] = update;
        probeStates[probeIndex].lastUpdatedFrame = currentFrame;
    }
}
