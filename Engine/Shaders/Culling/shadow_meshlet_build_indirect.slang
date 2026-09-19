struct ShadowMeshTaskIndirectCommand { uint groupCountX; uint groupCountY; uint groupCountZ; };
struct ShadowMeshletBuildUniforms { uint pageCount; uint meshletsPerPage; uint padding0; uint padding1; };
[[vk::binding(0, 0)]] StructuredBuffer<uint> visibleMeshletCounts;
[[vk::binding(1, 0)]] RWStructuredBuffer<ShadowMeshTaskIndirectCommand> commands;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> commandCounts;
[[vk::binding(3, 0)]] ConstantBuffer<ShadowMeshletBuildUniforms> culling;

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID) {
    const uint page = id.x;
    if (page >= culling.pageCount) return;
    const uint count = min(visibleMeshletCounts[page], culling.meshletsPerPage);
    commands[page] = ShadowMeshTaskIndirectCommand(count, 1u, 1u);
    commandCounts[page] = count == 0u ? 0u : 1u;
}
