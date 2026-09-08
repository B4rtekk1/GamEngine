import Grass.GrassTypes;

struct GPUSceneMeshRecord { uint4 draw; uint4 lod; };
struct GrassIndirectUniforms { uint visibleCapacity; uint binCapacity; uint compactBase; uint maxBins; float4 cameraPosition; };
[[vk::binding(0, 0)]] StructuredBuffer<GPUSceneMeshRecord> meshes;
[[vk::binding(1, 0)]] StructuredBuffer<uint> binCounts;
[[vk::binding(2, 0)]] StructuredBuffer<uint> binOffsets;
[[vk::binding(3, 0)]] RWStructuredBuffer<GrassDrawCommand> commands;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> drawCount;
[[vk::binding(5, 0)]] ConstantBuffer<GrassIndirectUniforms> uniforms;

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID)
{
    uint bin = id.x;
    if (bin >= uniforms.maxBins) return;
    uint count = min(binCounts[bin], uniforms.binCapacity);
    if (count == 0u) return;
    uint meshId = bin / 3u;
    uint lod = bin % 3u;
    GPUSceneMeshRecord mesh = meshes[meshId];
    GrassDrawCommand command;
    command.indexCount = lod == 2u && mesh.lod.x != 0u ? mesh.lod.x :
                         (lod == 1u && mesh.draw.w != 0u ? mesh.draw.w : mesh.draw.y);
    command.instanceCount = count;
    command.firstIndex = mesh.draw.x;
    command.vertexOffset = int(mesh.draw.z);
    // The scatter pass packs only populated bins.  Its prefix-sum offset,
    // rather than a fixed bin stride, is the matching source address.
    command.firstInstance = uniforms.compactBase + binOffsets[bin];
    commands[bin] = command;
    InterlockedMax(drawCount[0], bin + 1u);
}
