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

// 32 one-sided entry counts, the 36 packed slot offsets, then the 32
// two-sided entry counts (MeshletDrawTwoSidedCountBase).
[[vk::binding(0, 0)]] StructuredBuffer<uint> drawCounts;
// Three commands per slot, in sets of SlotCount: every entry, the one-sided
// entries only, the two-sided entries only (ForwardPass::drawMeshShaderMaterial).
[[vk::binding(1, 0)]] RWStructuredBuffer<MeshTasksIndirectCommand> commands;
static const uint TwoSidedCountBase = SlotCount + 36u;

MeshTasksIndirectCommand commandFor(uint count) {
    count = min(count, MaxWorkGroups);
    return count == 0u
        ? MeshTasksIndirectCommand(0u, 0u, 0u)
        : MeshTasksIndirectCommand(min(count, MeshletDrawRow), (count + MeshletDrawRow - 1u) / MeshletDrawRow, 1u);
}

[numthreads(32, 1, 1)]
[shader("compute")]
void main(uint3 threadId : SV_DispatchThreadID) {
    const uint slot = threadId.x;
    if (slot >= SlotCount) return;
    const uint capacity = drawCounts[SlotCount + 1u + slot] - drawCounts[SlotCount + slot];
    const uint oneSided = min(drawCounts[slot], capacity);
    const uint twoSided = min(drawCounts[TwoSidedCountBase + slot], capacity - oneSided);
    commands[slot] = commandFor(oneSided + twoSided);
    commands[SlotCount + slot] = commandFor(oneSided);
    commands[SlotCount * 2u + slot] = commandFor(twoSided);
}
