// Bins visible meshlets into indexed draws for the existing forward material
// pipelines. Each command addresses one expanded meshlet in the index heap.
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

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 threadId : SV_DispatchThreadID) {
    uint visibleCapacity, visibleStride;
    visibleMeshlets.GetDimensions(visibleCapacity, visibleStride);
    const uint index = threadId.x;
    if (index >= min(visibleCount[0], visibleCapacity)) return;

    const VisibleMeshlet visible = visibleMeshlets[index];
    const GPUSceneInstanceRecord instance = instances[visible.instanceId];
    const uint shaderSlot = materials[instance.idsAndFlags.y].data.w;
    if (shaderSlot >= 32u) return;

    uint commandCapacity, commandStride;
    commands.GetDimensions(commandCapacity, commandStride);
    const uint capacityPerSlot = commandCapacity / 32u;
    uint destination;
    InterlockedAdd(drawCounts[shaderSlot], 1u, destination);
    if (destination >= capacityPerSlot) return;

    const GpuMeshlet meshlet = meshlets[visible.meshletId];
    commands[shaderSlot * capacityPerSlot + destination] = IndexedDrawCommand(
        meshlet.range.w * 3u, 1u, meshlet.range.z * 3u, 0, instance.idsAndFlags.z);
}
