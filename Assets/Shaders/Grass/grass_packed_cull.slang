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
struct Uniforms { float4x4 viewProjection; float4 cameraPosition; uint clusterCount; uint _pad0; uint _pad1; uint _pad2; };
[[vk::binding(6, 0)]] ConstantBuffer<Uniforms> culling;

// Returns true only when the blade's bounding sphere is fully outside one
// frustum plane.  The offset transforms give conservative clip-space extents
// without needing to extract and normalize world-space frustum planes.
bool grassSphereOutsideFrustum(float3 center, float radius, float4x4 viewProjection)
{
    const float4 clipCenter = mul(float4(center, 1.0), viewProjection);
    const float4 clipX = mul(float4(radius, 0.0, 0.0, 0.0), viewProjection);
    const float4 clipY = mul(float4(0.0, radius, 0.0, 0.0), viewProjection);
    const float4 clipZ = mul(float4(0.0, 0.0, radius, 0.0), viewProjection);

    // An AABB around the sphere is deliberately used for the support values.
    // It is slightly conservative, but cannot reject a visible blade.
    const float4 clipExtent = abs(clipX) + abs(clipY) + abs(clipZ);
    return
        clipCenter.x + clipCenter.w < -(clipExtent.x + clipExtent.w) ||
        clipCenter.w - clipCenter.x < -(clipExtent.w + clipExtent.x) ||
        clipCenter.y + clipCenter.w < -(clipExtent.y + clipExtent.w) ||
        clipCenter.w - clipCenter.y < -(clipExtent.w + clipExtent.y) ||
        clipCenter.z < -clipExtent.z ||
        clipCenter.w - clipCenter.z < -(clipExtent.w + clipExtent.z);
}

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID)
{
    if (group.y >= visibleClusterCount[0]) return;
    uint clusterId = visibleClusters[group.y];
    if (clusterId >= culling.clusterCount) return;
    GrassPackedCluster cluster = clusters[clusterId];
    if (id.x >= cluster.instanceRange.y) return;
    uint instanceId = cluster.instanceRange.x + id.x;
    GrassPackedInstance instance = instances[instanceId];
    float3 position = grassPackedWorldPosition(instance, cluster);
    float radius = cluster.originExtent.w * grassPackedScale(instance);
    if (grassSphereOutsideFrustum(position, radius, culling.viewProjection)) return;
    uint destination;
    InterlockedAdd(visibleCount[0], 1u, destination);
    visibleInstances[destination] = instanceId;
}
