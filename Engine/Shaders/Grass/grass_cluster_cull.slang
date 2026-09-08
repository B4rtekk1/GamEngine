import Grass.GrassPackedData;

[[vk::binding(0, 0)]] StructuredBuffer<GrassPackedCluster> clusters;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> visibleClusters;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> visibleClusterCount;
// x and z are initialized once by the CPU; this pass resets y before dispatch
// and atomically grows it as clusters become visible.
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> bladeDispatch;
struct Uniforms {
    float4x4 viewProjection;
    float4 cameraPosition;
    float4 frustumPlanes[6];
    uint clusterCount;
    uint _pad0;
    uint _pad1;
    uint _pad2;
};
[[vk::binding(4, 0)]] ConstantBuffer<Uniforms> culling;

// 0 = outside, 1 = intersects a frustum boundary, 2 = fully inside.
uint classifySphere(float3 center, float radius) {
    bool fullyInside = true;
    for (uint planeIndex = 0; planeIndex < 6; ++planeIndex) {
        const float distance = dot(culling.frustumPlanes[planeIndex].xyz, center) +
                               culling.frustumPlanes[planeIndex].w;
        if (distance < -radius)
            return 0;
        fullyInside = fullyInside && distance >= radius;
    }
    return fullyInside ? 2 : 1;
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
    const uint classification = classifySphere(center, radius);
    if (classification == 0) return;
    uint slot; InterlockedAdd(visibleClusterCount[0], 1u, slot);
    // The high bit avoids an extra status buffer. Cluster IDs are naturally
    // far below 2^31; it marks a range that is wholly inside the frustum.
    visibleClusters[slot] = id.x | (classification == 2 ? 0x80000000u : 0u);
    uint ignored; InterlockedMax(bladeDispatch[1], slot + 1u, ignored);
}
