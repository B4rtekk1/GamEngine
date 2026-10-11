import Common.SceneData;

struct ShadowInstanceTransform { float4 row0; float4 row1; float4 row2; };
[[vk::binding(5, 0)]] StructuredBuffer<ShadowInstanceTransform> shadowTransforms;
[[vk::binding(6, 0)]] StructuredBuffer<uint> shadowMaterialOffsets;
// The compact transform pool is bounded. Oversized batches use the original
// scene streams instead of disappearing from the shadow map on overflow.
[[vk::binding(3, 0)]] StructuredBuffer<RendererInstanceData> shadowSourceInstances;
[[vk::binding(7, 0)]] StructuredBuffer<uint> shadowSourceIndices;

ShadowInstanceTransform loadShadowCaster(uint index, out uint materialOffset)
{
    if ((index & 0x80000000u) == 0)
    {
        materialOffset = shadowMaterialOffsets[index];
        return shadowTransforms[index];
    }
    const RendererInstanceData instance = shadowSourceInstances[shadowSourceIndices[index & 0x7fffffffu]];
    const float4 q = instance.rotation;
    const float3 s = instance.scaleBase.xyz;
    ShadowInstanceTransform transform;
    transform.row0 = float4(s.x * (1.0 - 2.0 * (q.y*q.y + q.z*q.z)),
        s.y * (2.0 * (q.x*q.y - q.z*q.w)), s.z * (2.0 * (q.x*q.z + q.y*q.w)), instance.positionMaterial.x);
    transform.row1 = float4(s.x * (2.0 * (q.x*q.y + q.z*q.w)),
        s.y * (1.0 - 2.0 * (q.x*q.x + q.z*q.z)), s.z * (2.0 * (q.y*q.z - q.x*q.w)), instance.positionMaterial.y);
    transform.row2 = float4(s.x * (2.0 * (q.x*q.z - q.y*q.w)),
        s.y * (2.0 * (q.y*q.z + q.x*q.w)), s.z * (1.0 - 2.0 * (q.x*q.x + q.y*q.y)), instance.positionMaterial.z);
    materialOffset = asuint(instance.positionMaterial.w);
    return transform;
}
