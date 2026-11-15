// Bins visible meshlets per material program slot for the mesh-shader forward
// path: one entry per meshlet, consumed by one mesh-shader workgroup each
// (Common/MeshletDraw.slang). Shares its descriptor layout with
// meshlet_build_indexed.slang; binding 5 holds the entries instead of
// indexed draw commands.
// meshletId: first meshlet in the low 24 bits, run length minus one above.
struct VisibleMeshlet { uint instanceId; uint meshletId; };
struct GPUSceneInstanceRecord {
    float4x4 worldMatrix;
    float4 localBoundsMin;
    float4 localBoundsMax;
    uint4 idsAndFlags;
    float4 displacementBoundsPadding;
};
struct GPUSceneMaterialRecord { uint4 data; };
// visibleInstance addresses RendererInstanceData, like an indexed meshlet
// draw's firstInstance. meshletId carries MeshletDrawEntryTwoSided
// (Common/MeshletDraw.slang) in its top bit.
struct MeshletDrawEntry { uint visibleInstance; uint meshletId; };

[[vk::binding(0, 0)]] StructuredBuffer<VisibleMeshlet> visibleMeshlets;
[[vk::binding(1, 0)]] StructuredBuffer<uint> visibleCount;
[[vk::binding(3, 0)]] StructuredBuffer<GPUSceneInstanceRecord> instances;
[[vk::binding(4, 0)]] StructuredBuffer<GPUSceneMaterialRecord> materials;
[[vk::binding(5, 0)]] RWStructuredBuffer<MeshletDrawEntry> drawList;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> drawCounts;
// 32 one-sided counts + 36 slot offsets (Culling::DrawBinOffsetCount).
static const uint TwoSidedCountBase = 68u;

// Every lane of a wave calls this together (`valid` false past the count), so
// the per-slot reservations can be combined into one atomic per slot and wave.
void binVisible(uint index, bool valid) {
    VisibleMeshlet visible = (VisibleMeshlet)0;
    uint shaderSlot = 0xffffffffu;
    uint visibleInstance = 0u;
    uint flags = 0u;
    if (valid) {
        visible = visibleMeshlets[index];
        const GPUSceneInstanceRecord instance = instances[visible.instanceId];
        shaderSlot = materials[instance.idsAndFlags.y].data.w;
        visibleInstance = instance.idsAndFlags.z;
        flags = instance.idsAndFlags.w;
    }
    // idsAndFlags.w: bit 1 foliage pipeline, bit 3 double-sided material.
    const bool twoSided = (flags & 10u) != 0u;
    const uint firstMeshlet = (visible.meshletId & 0x00ffffffu) | (twoSided ? 0x80000000u : 0u);
    const uint runLength = (visible.meshletId >> 24u) + 1u;
    // Material slots are few and visible runs arrive grouped by instance, so a
    // wave usually spans one or two slots: reserve each slot's entries with a
    // single atomic instead of one per run.
    // One reservation per (slot, sidedness): one-sided entries fill the
    // slot's range from the front, two-sided ones from the back
    // (MeshletDrawTwoSidedCountBase in Common/MeshletDraw.slang).
    const uint binKey = shaderSlot < 32u ? shaderSlot * 2u + (twoSided ? 1u : 0u) : 0xffffffffu;
    bool pending = shaderSlot < 32u;
    while (WaveActiveAnyTrue(pending)) {
        const uint key = WaveActiveMin(pending ? binKey : 0xffffffffu);
        const uint slot = key >> 1u;
        const bool back = (key & 1u) != 0u;
        const bool inSlot = pending && binKey == key;
        const uint length = inSlot ? runLength : 0u;
        const uint total = WaveActiveSum(length);
        const uint offsetInWave = WavePrefixSum(length);
        uint base = 0u;
        if (WaveIsFirstLane())
            InterlockedAdd(drawCounts[back ? TwoSidedCountBase + slot : slot], total, base);
        base = WaveReadLaneFirst(base);
        if (inSlot) {
            // drawCounts holds the 32 one-sided counts, then the packed slot
            // offsets: slot s owns entries [drawCounts[32 + s], drawCounts[33 + s]).
            const uint slotStart = drawCounts[32u + slot];
            const uint slotCapacity = drawCounts[33u + slot] - slotStart;
            const uint destination = base + offsetInWave;
            // The counts may pass the capacity; consumers clamp them. An
            // overflowing two-sided entry can then replace a one-sided one,
            // which the depth prepass's per-entry test still routes right.
            for (uint offset = 0u; offset < runLength && destination + offset < slotCapacity; ++offset)
                drawList[slotStart + (back ? slotCapacity - 1u - (destination + offset) : destination + offset)] =
                    MeshletDrawEntry(visibleInstance, firstMeshlet + offset);
            pending = false;
        }
    }
}

// The host launches at most BinGroups groups (renderer_backend_frame.inl):
// a grid-stride loop covers any visible count, so the launch no longer scales
// with the list capacity (one group per 64 meshlets of every instance) and
// cannot exceed maxComputeWorkGroupCount on very large scenes. Rounds keep
// every lane in the loop for the wave operations.
static const uint BinGroups = 1024u;

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 threadId : SV_DispatchThreadID) {
    uint visibleCapacity, visibleStride;
    visibleMeshlets.GetDimensions(visibleCapacity, visibleStride);
    const uint count = min(visibleCount[0], visibleCapacity);
    const uint stride = BinGroups * 64u;
    const uint rounds = (count + stride - 1u) / stride;
    for (uint round = 0u; round < rounds; ++round) {
        const uint index = round * stride + threadId.x;
        binVisible(index, index < count);
    }
}
