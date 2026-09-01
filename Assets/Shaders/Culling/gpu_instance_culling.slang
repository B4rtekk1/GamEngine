import Culling.CullingTypes;

struct GPUSceneInstanceRecord
{
    float4x4 worldMatrix;
    float4 localBoundsMin;
    float4 localBoundsMax;
    uint4 idsAndFlags;
};

[[vk::binding(0, 0)]] StructuredBuffer<GPUSceneInstanceRecord> instances;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> visibleInstances;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> visibleCount;
[[vk::binding(3, 0)]] ConstantBuffer<CullingUniformData> culling;

bool outsideFrustum(float4 clipPositions[8])
{
    bool outsideLeft = true, outsideRight = true, outsideBottom = true;
    bool outsideTop = true, outsideNear = true, outsideFar = true;
    for (uint i = 0; i < 8; ++i) {
        float4 clip = clipPositions[i];
        outsideLeft = outsideLeft && clip.x < -clip.w;
        outsideRight = outsideRight && clip.x > clip.w;
        outsideBottom = outsideBottom && clip.y < -clip.w;
        outsideTop = outsideTop && clip.y > clip.w;
        outsideNear = outsideNear && clip.z < 0.0;
        outsideFar = outsideFar && clip.z > clip.w;
    }
    return outsideLeft || outsideRight || outsideBottom || outsideTop || outsideNear || outsideFar;
}

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint instanceId = dispatchThreadId.x;
    uint instanceCount, instanceStride;
    instances.GetDimensions(instanceCount, instanceStride);
    if (instanceId >= instanceCount) return;
    const GPUSceneInstanceRecord instance = instances[instanceId];
    // Bit 0 is the renderer visibility/alive bit; inactive slots never enter the list.
    if ((instance.idsAndFlags.w & 1U) == 0U) return;
    float4 clipPositions[8];
    const float3 minimum = instance.localBoundsMin.xyz;
    const float3 maximum = instance.localBoundsMax.xyz;
    for (uint i = 0; i < 8; ++i) {
        const float3 localPosition = lerp(minimum, maximum, UNIT_AABB_CORNERS[i]);
        clipPositions[i] = mul(mul(float4(localPosition, 1.0), instance.worldMatrix), culling.viewProjection);
    }
    if (outsideFrustum(clipPositions)) return;
    uint destination;
    InterlockedAdd(visibleCount[0], 1, destination);
    uint maxVisibleInstances, visibleStride;
    visibleInstances.GetDimensions(maxVisibleInstances, visibleStride);
    if (destination < maxVisibleInstances) visibleInstances[destination] = instanceId;
}
