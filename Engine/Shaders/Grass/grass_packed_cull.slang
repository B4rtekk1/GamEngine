import Grass.GrassPackedData;

// One invocation per compact, packed blade.  This replaces the legacy chunk
// format for the new renderer path; it never reads GPUScene or
// RendererInstanceData.
[[vk::binding(0, 0)]] StructuredBuffer<GrassPackedInstance> instances;
[[vk::binding(1, 0)]] StructuredBuffer<GrassPackedCluster> clusters;
[[vk::binding(2, 0)]] StructuredBuffer<uint> visibleClusters;
[[vk::binding(3, 0)]] StructuredBuffer<uint> visibleClusterCount;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> visibleInstances;
[[vk::binding(5, 0)]] RWStructuredBuffer<uint> visibleCount;
struct Uniforms {
    float4x4 viewProjection;
    float4 cameraPosition;
    float4 frustumPlanes[6];
    uint clusterCount;
    uint _pad0;
    uint _pad1;
    uint _pad2;
};
[[vk::binding(6, 0)]] ConstantBuffer<Uniforms> culling;

// World-space planes replace four matrix transforms per blade.
bool grassSphereOutsideFrustum(float3 center, float radius)
{
    for (uint planeIndex = 0; planeIndex < 6; ++planeIndex) {
        const float4 plane = culling.frustumPlanes[planeIndex];
        if (dot(plane.xyz, center) + plane.w < -radius)
            return true;
    }
    return false;
}

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID)
{
    if (group.y >= visibleClusterCount[0]) return;
    uint clusterEntry = visibleClusters[group.y];
    bool fullyInside = (clusterEntry & 0x80000000u) != 0u;
    uint clusterId = clusterEntry & 0x7fffffffu;
    if (clusterId >= culling.clusterCount) return;
    GrassPackedCluster cluster = clusters[clusterId];
    if (id.x >= cluster.instanceRange.y) return;
    uint instanceId = cluster.instanceRange.x + id.x;
    GrassPackedInstance instance = instances[instanceId];
    float3 position = grassPackedWorldPosition(instance, cluster);
    float radius = cluster.originExtent.w * grassPackedScale(instance);
    if (!fullyInside && grassSphereOutsideFrustum(position, radius)) return;
    uint destination;
    InterlockedAdd(visibleCount[0], 1u, destination);
    visibleInstances[destination] = instanceId;
}
