import Grass.GrassPackedData;

// The source stream is already distance-classified for shadows.  This pass
// adds the missing cluster -> VSM-page overlap relation and writes a distinct
// indexed-indirect range for every page.
struct Draw { uint indexCount; uint instanceCount; uint firstIndex; int vertexOffset; uint firstInstance; };
struct Uniforms { uint pageCount; uint clusterCount; uint _pad0; uint _pad1; };
[[vk::binding(0, 0)]] StructuredBuffer<GrassPackedCluster> clusters;
[[vk::binding(1, 0)]] StructuredBuffer<Draw> sourceCommands;
[[vk::binding(2, 0)]] StructuredBuffer<float4x4> pageViewProjections;
[[vk::binding(3, 0)]] RWStructuredBuffer<Draw> pageCommands;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> pageDrawCounts;
[[vk::binding(5, 0)]] ConstantBuffer<Uniforms> uniforms;

bool overlapsPage(GrassPackedCluster cluster, float4x4 pageMatrix) {
    const float halfHeight = f16tof32(cluster.draw.w >> 16u);
    const float radius = length(float3(cluster.originExtent.z * 0.70710678,
                                       halfHeight + cluster.originExtent.w, 0.0));
    const float3 center = float3(cluster.originExtent.x + cluster.originExtent.z * 0.5,
                                 f16tof32(cluster.draw.w),
                                 cluster.originExtent.y + cluster.originExtent.z * 0.5);
    const float4 clip = mul(float4(center, 1.0), pageMatrix);
    // Conservative homogeneous clip-space sphere test. Page matrices are
    // exact VSM tile transforms, so this is the authoritative overlap data.
    const float w = abs(clip.w);
    const float r = radius * max(max(length(pageMatrix[0].xyz), length(pageMatrix[1].xyz)),
                                 length(pageMatrix[2].xyz));
    return !(clip.x < -w - r || clip.x > w + r ||
             clip.y < -w - r || clip.y > w + r ||
             clip.z < -r || clip.z > w + r);
}

[numthreads(64, 1, 1)] [shader("compute")]
void main(uint3 id : SV_DispatchThreadID) {
    const uint cluster = id.x;
    const uint page = id.y;
    if (cluster >= uniforms.clusterCount || page >= uniforms.pageCount) return;
    const uint output = page * uniforms.clusterCount + cluster;
    Draw command = sourceCommands[cluster];
    const bool visible = command.instanceCount != 0 &&
                         overlapsPage(clusters[cluster], pageViewProjections[page]);
    // The range is reused by later frame slots. Explicitly clear rejected
    // commands instead of relying only on the count: sparse cluster IDs mean
    // a count can still cover an old command at a lower index.
    if (!visible) command.instanceCount = 0;
    pageCommands[output] = command;
    if (!visible) return;
    uint ignored;
    InterlockedMax(pageDrawCounts[page], cluster + 1u, ignored);
}
