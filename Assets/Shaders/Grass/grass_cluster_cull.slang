import Grass.GrassPackedData;

[[vk::binding(0, 0)]] StructuredBuffer<GrassPackedCluster> clusters;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> visibleClusters;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> visibleClusterCount;
// x and z are initialized once by the CPU; this pass resets y before dispatch
// and atomically grows it as clusters become visible.
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> bladeDispatch;
struct Uniforms { float4x4 viewProjection; float4 cameraPosition; uint clusterCount; uint _pad0; uint _pad1; uint _pad2; };
[[vk::binding(4, 0)]] ConstantBuffer<Uniforms> culling;

bool outsideFrustum(float3 center, float radius, float4x4 viewProjection) {
    float4 clipCenter = mul(float4(center, 1.0), viewProjection);
    float4 clipX = mul(float4(radius, 0, 0, 0), viewProjection);
    float4 clipY = mul(float4(0, radius, 0, 0), viewProjection);
    float4 clipZ = mul(float4(0, 0, radius, 0), viewProjection);
    float4 extent = abs(clipX) + abs(clipY) + abs(clipZ);
    return clipCenter.x + clipCenter.w < -(extent.x + extent.w) ||
        clipCenter.w - clipCenter.x < -(extent.w + extent.x) ||
        clipCenter.y + clipCenter.w < -(extent.y + extent.w) ||
        clipCenter.w - clipCenter.y < -(extent.w + extent.y) ||
        clipCenter.z < -extent.z || clipCenter.w - clipCenter.z < -(extent.w + extent.z);
}

[numthreads(64, 1, 1)] [shader("compute")]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= culling.clusterCount) return;
    GrassPackedCluster cluster = clusters[id.x];
    float halfHeight = f16tof32(cluster.draw.w >> 16u);
    float radius = length(float3(cluster.originExtent.z * 0.70710678,
                                 halfHeight + cluster.originExtent.w, 0.0));
    float3 center = float3(cluster.originExtent.x + cluster.originExtent.z * 0.5,
                           f16tof32(cluster.draw.w),
                           cluster.originExtent.y + cluster.originExtent.z * 0.5);
    if (outsideFrustum(center, radius, culling.viewProjection)) return;
    uint slot; InterlockedAdd(visibleClusterCount[0], 1u, slot);
    visibleClusters[slot] = id.x;
    uint ignored; InterlockedMax(bladeDispatch[1], slot + 1u, ignored);
}
