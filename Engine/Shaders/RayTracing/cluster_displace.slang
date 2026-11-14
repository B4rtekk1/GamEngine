// Cluster template instantiation input for displaced geometry. Displacement
// moves vertices along their normal by the material's height map, so a mesh
// section displaced by one material is a fixed set of positions: each
// workgroup computes them for one cluster of one (section, material) variant
// and writes the VkClusterAccelerationStructureInstantiateClusterInfoNV that
// turns the section's shared cluster template into that variant's CLAS.
// Animated deformation would rerun only this pass and the instantiation.
import Common.SceneData;
import Common.VertexFrame;
import Common.Displacement;

struct GpuMeshlet { uint4 range; float4 bounds; float4 cone; uint4 material; };

[[vk::binding(2, 0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(0, 1)]] StructuredBuffer<GpuMeshlet> meshlets;
[[vk::binding(1, 1)]] StructuredBuffer<uint> meshletVertices;
[[vk::binding(2, 1)]] ByteAddressBuffer vertices;
[[vk::binding(3, 1)]] StructuredBuffer<uint4> clusterMeshlets;
// x: variant cluster (its ClusterID), y: base cluster, z: material base, w: position slot.
[[vk::binding(4, 1)]] StructuredBuffer<uint4> jobs;
[[vk::binding(5, 1)]] StructuredBuffer<uint2> templateAddresses;
[[vk::binding(6, 1)]] RWStructuredBuffer<float> positions;
[[vk::binding(7, 1)]] RWStructuredBuffer<uint> instantiateInfos;
struct Settings {
    uint64_t positionsAddress;
    uint jobCount;
    uint groupsPerRow;
};
[[vk::push_constant]] ConstantBuffer<Settings> settings;

static const uint MaxClusterVertices = 64u;
static const uint VertexStride = 32u; // sizeof(GpuVertex)
static const uint InstantiateWords = 8u;

[numthreads(64, 1, 1)] [shader("compute")]
void main(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID) {
    const uint job = groupId.x + groupId.y * settings.groupsPerRow;
    if (job >= settings.jobCount) return;
    const uint4 work = jobs[job];
    const uint4 base = clusterMeshlets[work.y];
    const uint4 range = meshlets[base.x].range;
    const uint lane = threadId.x;
    if (lane < min(range.y, MaxClusterVertices)) {
        const uint offset = meshletVertices[range.x + lane] * VertexStride;
        const float3 position = asfloat(vertices.Load3(offset));
        const float3 normal = decodeVertexNormal(vertices.Load(offset + 12u));
        const uint uv0 = vertices.Load(offset + 16u), uv1 = vertices.Load(offset + 20u);
        const MaterialData material = materials[work.z + vertices.Load(offset + 28u)];
        const float3 displaced = displaceMaterialPosition(
            material, position, normal, float2(f16tof32(uv0 & 0xffffu), f16tof32(uv0 >> 16u)),
            float2(f16tof32(uv1 & 0xffffu), f16tof32(uv1 >> 16u)));
        const uint destination = (work.w * MaxClusterVertices + lane) * 3u;
        positions[destination] = displaced.x;
        positions[destination + 1u] = displaced.y;
        positions[destination + 2u] = displaced.z;
    }
    if (lane == 0u) {
        const uint word = job * InstantiateWords;
        // The template's clusterID is its base cluster; the offset renames the
        // instance to the variant cluster so ClusterID() finds its record.
        instantiateInfos[word + 0u] = work.x - work.y;
        instantiateInfos[word + 1u] = 0u;                // geometryIndexOffset:24 | reserved:8
        const uint2 templateAddress = templateAddresses[base.z];
        instantiateInfos[word + 2u] = templateAddress.x;
        instantiateInfos[word + 3u] = templateAddress.y;
        const uint64_t vertexAddress = settings.positionsAddress + uint64_t(work.w) * (MaxClusterVertices * 12u);
        instantiateInfos[word + 4u] = uint(vertexAddress);
        instantiateInfos[word + 5u] = uint(vertexAddress >> 32u);
        instantiateInfos[word + 6u] = 12u;               // strideInBytes
        instantiateInfos[word + 7u] = 0u;
    }
}
