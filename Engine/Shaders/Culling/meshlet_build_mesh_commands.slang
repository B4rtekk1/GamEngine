// Turns the per-slot meshlet counts of meshlet_build_mesh_draws.slang into one
// VkDrawMeshTasksIndirectCommandEXT per material program slot. Workgroups are
// laid out in rows of MeshletDrawRow (Common/MeshletDraw.slang) because a
// single dimension is limited to 65535 groups; the mesh shader discards the
// padding groups of the last row.
struct MeshTasksIndirectCommand { uint groupCountX; uint groupCountY; uint groupCountZ; };

static const uint SlotCount = 32u;
static const uint MeshletDrawRow = 32768u;
// maxMeshWorkGroupTotalCount is at least 2^22.
static const uint MaxWorkGroups = 1u << 22u;

// 32 per-slot entry counts, then the packed slot offsets.
[[vk::binding(0, 0)]] StructuredBuffer<uint> drawCounts;
[[vk::binding(1, 0)]] RWStructuredBuffer<MeshTasksIndirectCommand> commands;

[numthreads(32, 1, 1)]
[shader("compute")]
void main(uint3 threadId : SV_DispatchThreadID) {
    const uint slot = threadId.x;
    if (slot >= SlotCount) return;
    const uint capacity = drawCounts[SlotCount + 1u + slot] - drawCounts[SlotCount + slot];
    const uint count = min(min(drawCounts[slot], capacity), MaxWorkGroups);
    commands[slot] = count == 0u
        ? MeshTasksIndirectCommand(0u, 0u, 0u)
        : MeshTasksIndirectCommand(min(count, MeshletDrawRow), (count + MeshletDrawRow - 1u) / MeshletDrawRow, 1u);
}
