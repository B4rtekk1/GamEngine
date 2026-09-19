// GPU-driven caster variant. Page selection comes from the page instance ID,
// while the page transform comes from the GPU-generated PageWork buffer.
import Common.SceneData;

struct PushConstants { float4x4 unused; };
struct PageWork
{
    float4x4 viewProjection;
    uint drawSlot;
    uint clipLevel;
    uint physicalPage;
    uint virtualPage;
};

[[vk::binding(5, 0)]] StructuredBuffer<RendererInstanceData> visibleInstances;
[[vk::binding(6, 0)]] StructuredBuffer<uint> visibleInstanceIndices;
[[vk::binding(8, 0)]] StructuredBuffer<PageWork> pageWork;
[[vk::push_constant]] ConstantBuffer<PushConstants> pushConstants;

struct ShadowVertexInput { [[vk::location(0)]] float3 position : POSITION; };
struct ShadowVertexOutput { float4 position : SV_Position; };

[shader("vertex")]
ShadowVertexOutput vertexMain(ShadowVertexInput input,
                              uint instanceIndex : SV_InstanceID,
                              uint drawId : SV_StartInstanceLocation)
{
    RendererInstanceData instance = visibleInstances[
        visibleInstanceIndices[instanceIndex]];
    float4 q = instance.rotation;
    float3x3 rotationMatrix = float3x3(
        1.0 - 2.0 * (q.y*q.y + q.z*q.z), 2.0 * (q.x*q.y + q.z*q.w), 2.0 * (q.x*q.z + q.y*q.w),
        2.0 * (q.x*q.y - q.z*q.w), 1.0 - 2.0 * (q.x*q.x + q.z*q.z), 2.0 * (q.y*q.z + q.x*q.w),
        2.0 * (q.x*q.z + q.y*q.w), 2.0 * (q.y*q.z - q.x*q.w), 1.0 - 2.0 * (q.x*q.x + q.y*q.y));
    float3 world = mul(input.position * instance.scaleBase.xyz, rotationMatrix) +
                   instance.positionMaterial.xyz;
    ShadowVertexOutput output;
    const PageWork work = pageWork[drawId];
    const float4 lightClip = mul(float4(world, 1.0), work.viewProjection);
    // The culling matrix produces page-local NDC.  Convert it to the physical
    // atlas tile here so one indirect draw can target a different tile without
    // CPU viewport/scissor changes.
    const uint tileX = work.physicalPage % 16u;
    const uint tileY = work.physicalPage / 16u;
    const float2 pageUv = lightClip.xy / lightClip.w * 0.5 + 0.5;
    const float2 atlasUv = (float2(tileX, tileY) + pageUv) / 16.0;
    output.position = float4(atlasUv * 2.0 - 1.0, lightClip.z / lightClip.w, 1.0);
    return output;
}
