// GPU-driven input for VK_NV_cluster_acceleration_structure: every meshlet
// becomes one triangle cluster (CLAS). A cluster needs its positions packed
// contiguously and tightly packed 8-bit local indices, while meshlets
// reference the shared GpuVertex heap through a vertex-index list and pack a
// triangle per 32-bit word. One workgroup converts one meshlet and writes the
// VkClusterAccelerationStructureBuildTriangleClusterInfoNV the indirect build
// consumes, so the CPU never touches meshlet geometry. Clusters that displaced
// instances also need get a cluster template (same topology, positions
// supplied later at instantiation).
struct GpuMeshlet { uint4 range; float4 bounds; float4 cone; uint4 material; };

[[vk::binding(0, 0)]] StructuredBuffer<GpuMeshlet> meshlets;
[[vk::binding(1, 0)]] StructuredBuffer<uint> meshletVertices;
[[vk::binding(2, 0)]] StructuredBuffer<uint> meshletTriangles;
[[vk::binding(3, 0)]] ByteAddressBuffer vertices;
// x: global meshlet, y: geometry index within the owning cluster BLAS,
// z: template slot or ~0.
[[vk::binding(4, 0)]] StructuredBuffer<uint4> clusterMeshlets;
[[vk::binding(5, 0)]] RWStructuredBuffer<float> clusterPositions;
[[vk::binding(6, 0)]] RWByteAddressBuffer clusterIndices;
[[vk::binding(7, 0)]] RWStructuredBuffer<uint> buildInfos;
// VkClusterAccelerationStructureBuildTriangleClusterTemplateInfoNV: the triangle
// cluster fields followed by instantiationBoundingBoxLimit.
[[vk::binding(8, 0)]] RWStructuredBuffer<uint> templateInfos;
struct Settings {
    uint64_t positionsAddress;
    uint64_t indicesAddress;
    uint clusterCount;
    uint groupsPerRow;
};
[[vk::push_constant]] ConstantBuffer<Settings> settings;

// Fixed per-cluster slots: meshlets hold at most 64 vertices and 126 triangles.
static const uint MaxClusterVertices = 64u;
static const uint ClusterIndexBytes = 384u;
static const uint VertexStride = 32u; // sizeof(GpuVertex)
static const uint BuildInfoWords = 16u;
static const uint TemplateInfoWords = 18u;
static const uint NoTemplate = 0xffffffffu;

[numthreads(64, 1, 1)] [shader("compute")]
void main(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID) {
    const uint cluster = groupId.x + groupId.y * settings.groupsPerRow;
    if (cluster >= settings.clusterCount) return;
    const uint4 source = clusterMeshlets[cluster];
    const uint4 range = meshlets[source.x].range;
    const uint vertexCount = min(range.y, MaxClusterVertices);
    const uint triangleCount = min(range.w, 126u);
    const uint lane = threadId.x;
    if (lane < vertexCount) {
        const uint vertex = meshletVertices[range.x + lane];
        const float3 position = asfloat(vertices.Load3(vertex * VertexStride));
        const uint destination = (cluster * MaxClusterVertices + lane) * 3u;
        clusterPositions[destination] = position.x;
        clusterPositions[destination + 1u] = position.y;
        clusterPositions[destination + 2u] = position.z;
    }
    // Byte b of the tight stream is corner b % 3 of triangle b / 3.
    const uint indexWords = (triangleCount * 3u + 3u) / 4u;
    for (uint word = lane; word < indexWords; word += 64u) {
        uint packed = 0u;
        [unroll] for (uint byteInWord = 0u; byteInWord < 4u; ++byteInWord) {
            const uint byteIndex = word * 4u + byteInWord;
            if (byteIndex >= triangleCount * 3u) break;
            const uint triangle = meshletTriangles[range.z + byteIndex / 3u];
            packed |= ((triangle >> ((byteIndex % 3u) * 8u)) & 0xffu) << (byteInWord * 8u);
        }
        clusterIndices.Store(cluster * ClusterIndexBytes + word * 4u, packed);
    }
    if (lane == 0u) {
        const uint64_t indexAddress = settings.indicesAddress + uint64_t(cluster) * ClusterIndexBytes;
        const uint64_t vertexAddress = settings.positionsAddress + uint64_t(cluster) * (MaxClusterVertices * 12u);
        const uint base = cluster * BuildInfoWords;
        buildInfos[base + 0u] = cluster;                 // clusterID
        buildInfos[base + 1u] = 0u;                      // clusterFlags
        // triangleCount:9 | vertexCount:9 | positionTruncateBitCount:6 | indexType:4 (8-bit = 1) | ommIndexType:4
        buildInfos[base + 2u] = triangleCount | (vertexCount << 9u) | (1u << 24u);
        // geometryIndex:24 | reserved:5 | geometryFlags:3 (opaque = 4)
        buildInfos[base + 3u] = (source.y & 0xffffffu) | (4u << 29u);
        buildInfos[base + 4u] = 0u | (12u << 16u);       // indexBufferStride (tight) | vertexBufferStride
        buildInfos[base + 5u] = 0u;                      // geometryIndexAndFlags / OMM index strides
        buildInfos[base + 6u] = uint(indexAddress);
        buildInfos[base + 7u] = uint(indexAddress >> 32u);
        buildInfos[base + 8u] = uint(vertexAddress);
        buildInfos[base + 9u] = uint(vertexAddress >> 32u);
        [unroll] for (uint word = 10u; word < BuildInfoWords; ++word)
            buildInfos[base + word] = 0u;
        if (source.z != NoTemplate) {
            // The base positions are a placement hint; templates keep topology only.
            const uint templateBase = source.z * TemplateInfoWords;
            [unroll] for (uint word = 0u; word < BuildInfoWords; ++word)
                templateInfos[templateBase + word] = buildInfos[base + word];
            templateInfos[templateBase + 16u] = 0u;
            templateInfos[templateBase + 17u] = 0u;
        }
    }
}
