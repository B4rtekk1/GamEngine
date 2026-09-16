// Converts the compacted meshlet stream into the single indirect mesh-task
// dispatch consumed by the mesh-shader renderer. The count buffer deliberately
// contains either zero or one command: material/pipeline binning expands this
// into several commands later without changing the consumer contract.

struct VisibleMeshlet { uint instanceId; uint meshletId; };
struct MeshTaskIndirectCommand { uint groupCountX; uint groupCountY; uint groupCountZ; };

[[vk::binding(0, 0)]] StructuredBuffer<VisibleMeshlet> visibleMeshlets;
[[vk::binding(1, 0)]] StructuredBuffer<uint> visibleMeshletCount;
[[vk::binding(2, 0)]] RWStructuredBuffer<MeshTaskIndirectCommand> commands;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> commandCount;

[numthreads(1, 1, 1)]
[shader("compute")]
void main() {
    uint capacity, stride;
    visibleMeshlets.GetDimensions(capacity, stride);
    const uint count = min(visibleMeshletCount[0], capacity);
    commands[0] = MeshTaskIndirectCommand(count, 1u, 1u);
    commandCount[0] = count == 0u ? 0u : 1u;
}
