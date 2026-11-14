// Mesh-shader VSM casters: turns the per-page meshlet entry counts of
// shadow_meshlet_refine.slang (SHADOW_MESH_TASKS) into one
// VkDrawMeshTasksIndirectCommandEXT per page, one workgroup per entry.
// Shares the refine pass's descriptor set and push constants.
struct MeshTasksIndirectCommand { uint groupCountX; uint groupCountY; uint groupCountZ; };

struct RefineSettings
{
    uint pageCount;
    uint maxDrawCount;
    // Entry capacity of one page; the refine counter may pass it.
    uint meshletCommandsPerPage;
    uint meshletCount;
};

[[vk::binding(7, 0)]] StructuredBuffer<uint> meshletCommandCounts;
[[vk::binding(9, 0)]] RWStructuredBuffer<MeshTasksIndirectCommand> taskCommands;
[[vk::push_constant]] ConstantBuffer<RefineSettings> settings;

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 threadId : SV_DispatchThreadID)
{
    const uint page = threadId.x;
    if (page >= settings.pageCount) return;
    const uint count = min(meshletCommandCounts[page], settings.meshletCommandsPerPage);
    taskCommands[page] = count == 0u ? MeshTasksIndirectCommand(0u, 0u, 0u)
                                     : MeshTasksIndirectCommand(count, 1u, 1u);
}
