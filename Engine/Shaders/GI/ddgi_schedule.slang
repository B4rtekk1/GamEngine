import GI.ddgi_common;
[[vk::push_constant]] DDGIConstants ddgi;
[[vk::binding(11, 1)]] RWStructuredBuffer<DDGIProbeState> probeStates;
[[vk::binding(12, 1)]] RWStructuredBuffer<DDGIProbeUpdate> probeUpdateList;

groupshared uint scores[DDGI_PROBES];

[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex) {
    for (uint i = groupIndex; i < DDGI_PROBES; i += 256u) {
        DDGIProbeState state = probeStates[i];
        if (ddgi.updateControl.x > 0.5) {
            state.variability = 1.0;
            state.updateCount = 0u;
            state.stableUpdates = 0u;
            probeStates[i] = state;
        }
        const uint age = min(uint(ddgi.scrollDeltaFrame.w) - state.lastUpdatedFrame, 4095u);
        const bool converged = state.updateCount >= DDGI_MIN_CONVERGENCE_UPDATES &&
                               state.stableUpdates >= DDGI_STABLE_UPDATES;
        // Converged probes leave the update list; exposed probes stay first.
        scores[i] = (uint(!converged) << 24u) | (uint(state.state == 2u) << 23u) |
                    (age << 11u) | (2047u - i);
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
        const bool eligible = (scores[groupIndex] & (1u << 24u)) != 0u;
        const uint probeIndex = eligible ? 2047u - (scores[groupIndex] & 2047u) : DDGI_PROBES;
        const uint currentFrame = uint(ddgi.scrollDeltaFrame.w);
        DDGIProbeUpdate update;
        update.probeIndex = probeIndex;
        update.relocated = 0u;
        update.framesSinceLastUpdate = 0u;
        if (eligible) {
            update.framesSinceLastUpdate = max(
                currentFrame - probeStates[probeIndex].lastUpdatedFrame, 1u);
            probeStates[probeIndex].lastUpdatedFrame = currentFrame;
        }
        probeUpdateList[groupIndex] = update;
    }
}
