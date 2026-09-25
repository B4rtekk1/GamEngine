import GI.ddgi_common;
[[vk::push_constant]] DDGIConstants ddgi;
[[vk::binding(11, 1)]] RWStructuredBuffer<DDGIProbeState> probeStates;
[[vk::binding(12, 1)]] RWStructuredBuffer<uint> probeUpdateList;

groupshared uint scores[DDGI_PROBES];
groupshared uint indices[DDGI_PROBES];

uint priority(uint probeIndex) {
    const DDGIProbeState probe = probeStates[probeIndex];
    if (probe.state == 2u) return 3u;
    if (probe.variability > 0.25) return 2u;
    const uint3 logical = ddgiLogicalGrid(probeIndex, ddgi.scrollOffsetRays.xyz);
    for (uint axis = 0u; axis < 3u; ++axis) {
        for (int sign = -1; sign <= 1; sign += 2) {
            int3 adjacent = int3(logical);
            adjacent[axis] += sign;
            if (any(adjacent < 0) || any(adjacent >= int3(DDGI_COUNTS))) continue;
            if (probeStates[ddgiPhysicalIndex(uint3(adjacent), ddgi.scrollOffsetRays.xyz)].state == 2u)
                return 2u;
        }
    }
    return uint(uint(ddgi.scrollDeltaFrame.w) - probe.lastUpdatedFrame > 8u);
}

[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex) {
    for (uint i = groupIndex; i < DDGI_PROBES; i += 256u) {
        const DDGIProbeState state = probeStates[i];
        const uint age = min(uint(ddgi.scrollDeltaFrame.w) - state.lastUpdatedFrame, 4095u);
        const uint variation = uint(saturate(state.variability) * 255.0);
        scores[i] = (priority(i) << 29u) | (age << 17u) | (variation << 9u) | (511u - (i & 511u));
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
        probeUpdateList[groupIndex] = probeIndex;
        probeStates[probeIndex].lastUpdatedFrame = uint(ddgi.scrollDeltaFrame.w);
    }
}
