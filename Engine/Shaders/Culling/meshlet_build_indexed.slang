// Bins visible meshlets into indexed draws for the existing forward material
// pipelines. Each command addresses one expanded meshlet in the index heap.
// meshletId: first meshlet in the low 24 bits, run length minus one above.
struct VisibleMeshlet { uint instanceId; uint meshletId; };
struct GpuMeshlet { uint4 range; float4 bounds; float4 cone; uint4 material; };
struct GPUSceneInstanceRecord {
    float4x4 worldMatrix;
    float4 localBoundsMin;
    float4 localBoundsMax;
    uint4 idsAndFlags;
    float4 displacementBoundsPadding;
};
struct GPUSceneMaterialRecord { uint4 data; };
struct IndexedDrawCommand {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};

[[vk::binding(0, 0)]] StructuredBuffer<VisibleMeshlet> visibleMeshlets;
[[vk::binding(1, 0)]] StructuredBuffer<uint> visibleCount;
[[vk::binding(2, 0)]] StructuredBuffer<GpuMeshlet> meshlets;
[[vk::binding(3, 0)]] StructuredBuffer<GPUSceneInstanceRecord> instances;
[[vk::binding(4, 0)]] StructuredBuffer<GPUSceneMaterialRecord> materials;
[[vk::binding(5, 0)]] RWStructuredBuffer<IndexedDrawCommand> commands;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> drawCounts;

// `index` is below the clamped visible count.
void binVisible(uint index) {
    const VisibleMeshlet visible = visibleMeshlets[index];
    const GPUSceneInstanceRecord instance = instances[visible.instanceId];
    const uint shaderSlot = materials[instance.idsAndFlags.y].data.w;
    if (shaderSlot >= 32u) return;

    // drawCounts holds the 32 per-slot counts, then the packed slot offsets:
    // slot s owns commands [drawCounts[32 + s], drawCounts[33 + s]).
    const uint slotStart = drawCounts[32u + shaderSlot];
    const uint slotCapacity = drawCounts[33u + shaderSlot] - slotStart;
    uint destination;
    InterlockedAdd(drawCounts[shaderSlot], 1u, destination);
    if (destination >= slotCapacity) return;

    // Culling only merges meshlets whose triangle ranges are contiguous.
    const uint firstMeshlet = visible.meshletId & 0x00ffffffu;
    const uint lastMeshlet = firstMeshlet + (visible.meshletId >> 24u);
    // material.y: the meshlet's first index in the shared index heap.
    const uint firstIndex = meshlets[firstMeshlet].material.y;
    const GpuMeshlet last = meshlets[lastMeshlet];
    commands[slotStart + destination] = IndexedDrawCommand(
        last.material.y + last.range.w * 3u - firstIndex, 1u, firstIndex, 0, instance.idsAndFlags.z);
}

// The host launches at most BinGroups groups (renderer_backend_frame.inl):
// a grid-stride loop covers any visible count, so the launch no longer scales
// with the list capacity (one group per 64 meshlets of every instance) and
// cannot exceed maxComputeWorkGroupCount on very large scenes.
static const uint BinGroups = 1024u;

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 threadId : SV_DispatchThreadID) {
    uint visibleCapacity, visibleStride;
    visibleMeshlets.GetDimensions(visibleCapacity, visibleStride);
    const uint count = min(visibleCount[0], visibleCapacity);
    for (uint index = threadId.x; index < count; index += BinGroups * 64u)
        binVisible(index);
}
