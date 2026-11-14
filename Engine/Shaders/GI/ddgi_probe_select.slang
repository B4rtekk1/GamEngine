// Builds this frame's probe update list on the GPU so the trace is sized by
// vkCmdTraceRaysIndirectKHR and never spends rays on idle probes. Probes are
// refreshed round-robin (one bucket of `updateInterval` per frame); a probe
// that scrolled into the volume is reset and traced immediately; probes
// classified as buried in geometry are re-tested only every eighth cycle.
//
// Probe cache: a probe that has absorbed enough updates for its hysteresis to
// converge goes to sleep and keeps its cached irradiance. It is woken when the
// lighting changes (wake.x) or when something moved inside one of the wake
// boxes, and otherwise refreshed only every 64th cycle as a safety net for
// changes the CPU cannot see (an edited emissive material, for instance).
//
// Coarser cascades light only distant surfaces and scroll rarely, so each one
// is refreshed a quarter as often as the next finer cascade (the blend
// shortens its hysteresis to keep the response time reasonable).
import Common.DdgiCommon;

[[vk::binding(0, 0)]] ConstantBuffer<DdgiVolume> volume;
// xyz: world grid coordinate stored at the last update, w: flags.
[[vk::binding(1, 0)]] RWStructuredBuffer<int4> probeData;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> probeList;
// VkTraceRaysIndirectCommandKHR at word 0, VkDispatchIndirectCommand at word 4.
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> indirectArgs;
struct Settings { uint updateInterval; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;

static const int ProbeValid = 1, ProbeInactive = 2, ProbeReset = 4, ProbeSleeping = 8, ProbeDeactivating = 16;
// Bits 8..15 count updates since the probe was last disturbed.
static const int ProbeUpdateCountMask = 0xff00;
// A moved object changes the light of probes that can see it, not only of
// those inside its bounds.
static const float WakeMarginInProbes = 2.0;

bool insideWakeBox(float3 position, float spacing) {
    const float margin = WakeMarginInProbes * spacing;
    for (uint box = 0u; box < min(volume.wake.y, 16u); ++box) {
        const float3 low = volume.wakeBoxes[box * 2u].xyz - margin;
        const float3 high = volume.wakeBoxes[box * 2u + 1u].xyz + margin;
        if (all(position >= low) && all(position <= high)) return true;
    }
    return false;
}

[numthreads(64, 1, 1)] [shader("compute")]
void main(uint3 id : SV_DispatchThreadID) {
    const uint probe = id.x;
    const uint probesPerCascade = uint(volume.probeCounts.w);
    const uint cascade = probe / probesPerCascade;
    if (cascade >= ddgiCascadeCount(volume)) return;
    const uint local = probe - cascade * probesPerCascade;
    const int3 counts = volume.probeCounts.xyz;
    const int3 grid = ddgiGridFromStorage(volume, cascade, ddgiStorageFromIndex(local, counts));
    const float spacing = ddgiCascadeSpacing(volume, cascade);
    int4 data = probeData[probe];
    const bool entered = (data.w & ProbeValid) == 0 || any(data.xyz != grid);
    if (entered) {
        // A new world position: its history and classification are void.
        data = int4(grid, ProbeReset);
        probeData[probe] = data;
    } else if ((data.w & (ProbeSleeping | ProbeInactive | ProbeDeactivating | ProbeUpdateCountMask)) != 0 &&
               (volume.wake.x != 0u || insideWakeBox(float3(grid) * spacing, spacing))) {
        // Woken probes converge again from their cached value and are
        // re-classified, since moved geometry may have buried or freed them.
        data.w &= ~(ProbeSleeping | ProbeInactive | ProbeDeactivating | ProbeUpdateCountMask);
        probeData[probe] = data;
    }
    const uint frame = volume.atlasInfo.w;
    const uint interval = max(settings.updateInterval, 1u) << (2u * cascade);
    const bool due = (local + frame) % interval == 0u;
    const uint cycle = local / interval + frame / interval;
    const bool inactive = (data.w & ProbeInactive) != 0;
    const bool sleeping = (data.w & ProbeSleeping) != 0;
    const bool retest = cycle % 8u == 0u;
    const bool safetyRefresh = cycle % 64u == 0u;
    if (!entered && (!due || (inactive && !retest) || (sleeping && !safetyRefresh))) return;
    uint slot;
    InterlockedAdd(indirectArgs[1], 1u, slot);
    InterlockedAdd(indirectArgs[4], 1u);
    probeList[slot] = probe;
}
