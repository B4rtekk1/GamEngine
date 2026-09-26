import Common.SceneData;
import GI.ddgi_common;
[[vk::push_constant]] DDGIConstants ddgi;
[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(9, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> probeData;
[[vk::binding(11, 1)]] RWStructuredBuffer<DDGIProbeState> probeStates;
[[vk::binding(12, 1)]] RWStructuredBuffer<DDGIProbeUpdate> probeUpdateList;
[[vk::binding(19, 1)]] RWStructuredBuffer<uint> dispatchCommands;

groupshared uint pendingUpdates[256];

[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex) {
    uint hasPendingUpdate = 0u;
    for (uint i = groupIndex; i < DDGI_PROBES; i += 256u) {
        DDGIProbeState state = probeStates[i];
        if (ddgi.updateControl.x > 0.5) {
            state.variability = 1.0;
            state.updateCount = 0u;
            state.stableUpdates = 0u;
            probeStates[i] = state;
        }
        const bool converged = state.updateCount >= DDGI_MIN_CONVERGENCE_UPDATES &&
                               state.stableUpdates >= DDGI_STABLE_UPDATES;
        hasPendingUpdate |= uint(!converged);
    }
    pendingUpdates[groupIndex] = hasPendingUpdate;
    GroupMemoryBarrierWithGroupSync();
    // Pause tracing and blending only when the whole cascade has converged.
    for (uint stride = 128u; stride > 0u; stride >>= 1u) {
        if (groupIndex < stride)
            pendingUpdates[groupIndex] |= pendingUpdates[groupIndex + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if (groupIndex == 0u) {
        const uint count = pendingUpdates[0] != 0u
            ? min(ddgiUpdateCount(ddgi), DDGI_PROBES - uint(ddgi.updateControl.y)) : 0u;
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
    if (groupIndex < ddgiUpdateCount(ddgi)) {
        // A sweep covers each physical probe exactly once before publication.
        const uint scheduledIndex = uint(ddgi.updateControl.y) + groupIndex;
        const bool eligible = pendingUpdates[0] != 0u && scheduledIndex < DDGI_PROBES;
        const uint probeIndex = eligible ? scheduledIndex : DDGI_PROBES;
        const uint currentFrame = uint(ddgi.scrollDeltaFrame.w);
        DDGIProbeUpdate update;
        update.probeIndex = probeIndex;
        update.relocated = 0u;
        update.framesSinceLastUpdate = 0u;
        update.padding = 0u;
        update.localLightIndices = uint4(0xffffffffu);
        if (eligible) {
            update.framesSinceLastUpdate = max(
                currentFrame - probeStates[probeIndex].lastUpdatedFrame, 1u);
            probeStates[probeIndex].lastUpdatedFrame = currentFrame;
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
