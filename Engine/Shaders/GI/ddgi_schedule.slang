import GI.ddgi_common;
[[vk::push_constant]] DDGIConstants ddgi;
[[vk::binding(11, 1)]] RWStructuredBuffer<DDGIProbeState> probeStates;
[[vk::binding(12, 1)]] RWStructuredBuffer<DDGIProbeUpdate> probeUpdateList;

groupshared uint scores[DDGI_PROBES];

[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex) {
    for (uint i = groupIndex; i < DDGI_PROBES; i += 256u) {
        const DDGIProbeState state = probeStates[i];
        const uint age = min(uint(ddgi.scrollDeltaFrame.w) - state.lastUpdatedFrame, 4095u);
        // Newly exposed probes first, then the least recently updated probes.
        scores[i] = (uint(state.state == 2u) << 23u) | (age << 11u) | (2047u - i);
    }
    GroupMemoryBarrierWithGroupSync();
    // Sort 2048 probe keys in shared memory; only the cascade's quota is traced.
    for (uint width = 2u; width <= DDGI_PROBES; width <<= 1u) {
        for (uint stride = width >> 1u; stride > 0u; stride >>= 1u) {
            for (uint pair = groupIndex; pair < DDGI_PROBES / 2u; pair += 256u) {
                // Insert a zero at the stride bit: each lane owns one pair.
                const uint i = (pair / stride) * (2u * stride) + pair % stride;
                const uint peer = i + stride;
                const bool descending = (i & width) == 0u;
                if ((scores[i] < scores[peer]) == descending) {
                    const uint score = scores[i]; scores[i] = scores[peer]; scores[peer] = score;
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }
    if (groupIndex < ddgiUpdateCount(ddgi)) {
        // The unique tie-breaker already encodes the physical probe index.
        const uint probeIndex = 2047u - (scores[groupIndex] & 2047u);
        const uint currentFrame = uint(ddgi.scrollDeltaFrame.w);
        DDGIProbeUpdate update;
        update.probeIndex = probeIndex;
        update.relocated = 0u;
        update.framesSinceLastUpdate = max(
            currentFrame - probeStates[probeIndex].lastUpdatedFrame, 1u);
        probeUpdateList[groupIndex] = update;
        probeStates[probeIndex].lastUpdatedFrame = currentFrame;
    }
}
