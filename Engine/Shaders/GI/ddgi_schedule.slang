import Common.SceneData;
import GI.ddgi_common;
[[vk::push_constant]] DDGIConstants ddgi;
[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(9, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> probeData;
[[vk::binding(11, 1)]] RWStructuredBuffer<DDGIProbeState> probeStates;
[[vk::binding(12, 1)]] RWStructuredBuffer<DDGIProbeUpdate> probeUpdateList;
[[vk::binding(19, 1)]] RWStructuredBuffer<uint> dispatchCommands;

groupshared uint4 tierCounts[256];
groupshared uint selectedProbes[256];
groupshared uint4 tierBudget;
groupshared uint selectedCount;

uint scheduledProbe(uint scheduledIndex) {
    // Visit all eight parity phases, with dispersed cells within each phase.
    const uint phase = scheduledIndex >> 8u;
    const uint cell = ((scheduledIndex & 255u) * 73u + 19u) & 255u;
    const uint3 grid = uint3(
        2u * (cell & 7u) + (phase & 1u),
        2u * ((cell >> 3u) & 3u) + ((phase >> 1u) & 1u),
        2u * (cell >> 5u) + ((phase >> 2u) & 1u));
    return (grid.z * DDGI_COUNTS.y + grid.y) * DDGI_COUNTS.x + grid.x;
}

uint updateTier(DDGIProbeState state, uint currentFrame) {
    const bool converged = state.updateCount >= DDGI_MIN_CONVERGENCE_UPDATES &&
                           state.stableUpdates >= DDGI_STABLE_UPDATES;
    if (converged) return 4u;
    // Fresh, exposed, relocated and invalid probes need prompt validation.
    if (state.updateCount == 0u || state.state != 1u) return 0u;
    if (state.variability >= DDGI_VARIABILITY_THRESHOLD) return 1u;
    if (currentFrame - state.lastUpdatedFrame >= 32u) return 2u;
    return 3u;
}

[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex) {
    const uint currentFrame = uint(ddgi.scrollDeltaFrame.w);
    const uint budget = ddgiUpdateCount(ddgi);
    const uint start = uint(ddgi.updateControl.y);
    uint4 counts = uint4(0u);
    for (uint i = 0u; i < DDGI_PROBES / 256u; ++i) {
        const uint probeIndex = scheduledProbe((start + groupIndex * 8u + i) % DDGI_PROBES);
        DDGIProbeState state = probeStates[probeIndex];
        if (ddgi.updateControl.x > 0.5) {
            state.variability = 1.0;
            state.updateCount = 0u;
            state.stableUpdates = 0u;
            probeStates[probeIndex] = state;
        }
        const uint tier = updateTier(state, currentFrame);
        if (tier < 4u) ++counts[tier];
    }
    tierCounts[groupIndex] = counts;
    GroupMemoryBarrierWithGroupSync();
    // Inclusive prefix counts give every pending probe a unique slot in its tier.
    for (uint stride = 1u; stride < 256u; stride <<= 1u) {
        const uint4 preceding = groupIndex >= stride
            ? tierCounts[groupIndex - stride] : uint4(0u);
        GroupMemoryBarrierWithGroupSync();
        tierCounts[groupIndex] += preceding;
        GroupMemoryBarrierWithGroupSync();
    }
    if (groupIndex == 0u) {
        const uint4 total = tierCounts[255];
        // Reserve room for less urgent probes so persistently invalid or noisy
        // probes cannot consume the entire budget. Fill remaining slots by tier.
        uint4 selected = uint4(0u);
        selected.w = min(total.w, budget / 8u);
        selected.z = min(total.z, budget / 8u);
        selected.y = min(total.y, budget / 8u);
        selected.x = min(total.x, budget - selected.y - selected.z - selected.w);
        selected.y = min(total.y, budget - selected.x - selected.z - selected.w);
        selected.z = min(total.z, budget - selected.x - selected.y - selected.w);
        selected.w = min(total.w, budget - selected.x - selected.y - selected.z);
        tierBudget = selected;
        const uint count = selected.x + selected.y + selected.z + selected.w;
        selectedCount = count;
        const uint randomRays = ddgiRandomRayCount(ddgi, 0u);
        dispatchCommands[0] = (DDGI_FIXED_RAYS + 7u) / 8u;
        dispatchCommands[1] = (count + 7u) / 8u;
        dispatchCommands[2] = 1u;
        dispatchCommands[3] = (count + 63u) / 64u;
        dispatchCommands[4] = 1u;
        dispatchCommands[5] = 1u;
        dispatchCommands[6] = (randomRays + 7u) / 8u;
        dispatchCommands[7] = (count + 7u) / 8u;
        dispatchCommands[8] = 1u;
        dispatchCommands[9] = 1u;
        dispatchCommands[10] = 1u;
        dispatchCommands[11] = count;
    }
    GroupMemoryBarrierWithGroupSync();
    uint4 localCounts = uint4(0u);
    const uint4 previousCounts = groupIndex > 0u ? tierCounts[groupIndex - 1u] : uint4(0u);
    for (uint i = 0u; i < DDGI_PROBES / 256u; ++i) {
        const uint probeIndex = scheduledProbe((start + groupIndex * 8u + i) % DDGI_PROBES);
        const uint tier = updateTier(probeStates[probeIndex], currentFrame);
        if (tier >= 4u) continue;
        const uint rank = previousCounts[tier] + localCounts[tier]++;
        if (rank >= tierBudget[tier]) continue;
        const uint offset = tier == 0u ? 0u :
            tier == 1u ? tierBudget.x :
            tier == 2u ? tierBudget.x + tierBudget.y :
                         tierBudget.x + tierBudget.y + tierBudget.z;
        selectedProbes[offset + rank] = probeIndex;
    }
    GroupMemoryBarrierWithGroupSync();
    if (groupIndex < budget) {
        const bool eligible = groupIndex < selectedCount;
        const uint probeIndex = eligible ? selectedProbes[groupIndex] : DDGI_PROBES;
        DDGIProbeUpdate update;
        update.probeIndex = probeIndex;
        update.relocated = 0u;
        update.framesSinceLastUpdate = 0u;
        update.padding = 0u;
        update.localLightIndices = uint4(0xffffffffu);
        if (eligible) {
            update.framesSinceLastUpdate = max(
                currentFrame - probeStates[probeIndex].lastUpdatedFrame, 1u);
            const float3 probePosition = ddgiPosition(probeIndex, ddgi.originSpacing.xyz,
                ddgi.originSpacing.w, ddgi.scrollOffsetRays.xyz) +
                probeData[int2(ddgiProbePixel(probeIndex))].xyz;
            float bestScores[4] = {0.0, 0.0, 0.0, 0.0};
            for (uint lightIndex = 0u; lightIndex < min(frame.localLightCount, 32u); ++lightIndex) {
                const LocalLightData light = frame.localLights[lightIndex];
                const float distance = length(light.positionRange.xyz - probePosition);
                // Include lights near the probe's cell even when the probe centre
                // lies just outside their range.
                const float effectiveDistance = max(distance - ddgi.originSpacing.w, 0.0);
                if (effectiveDistance >= light.positionRange.w) continue;
                const float falloff = saturate(1.0 - effectiveDistance / light.positionRange.w);
                const float luminance = dot(light.colorIntensity.rgb,
                    float3(0.2126, 0.7152, 0.0722));
                const float score = light.colorIntensity.w * luminance * falloff * falloff /
                    max(effectiveDistance * effectiveDistance, 0.01);
                for (uint rank = 0u; rank < 4u; ++rank) {
                    if (score <= bestScores[rank]) continue;
                    for (int shift = 3; shift > int(rank); --shift) {
                        bestScores[shift] = bestScores[shift - 1];
                        update.localLightIndices[shift] = update.localLightIndices[shift - 1];
                    }
                    bestScores[rank] = score;
                    update.localLightIndices[rank] = lightIndex;
                    break;
                }
            }
        }
        probeUpdateList[groupIndex] = update;
    }
    GroupMemoryBarrierWithGroupSync();
    // Fade freshly retraced probes into the cascade over the next few frames.
    for (uint i = groupIndex; i < DDGI_PROBES; i += 256u) {
        const int2 pixel = int2(ddgiProbePixel(i));
        float4 probe = probeData[pixel];
        if (probe.w > 1.0 && probe.w < 1.5) {
            probe.w = max(1.0, probe.w - DDGI_REACTIVATION_STEP);
            probeData[pixel] = probe;
        }
    }
}
