import Common.SceneData;
import Common.Displacement;

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(2, 0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(5, 0)]] StructuredBuffer<RendererInstanceData> visibleInstances;
[[vk::binding(6, 0)]] StructuredBuffer<uint> visibleInstanceIndices;
#ifndef OUTLINE_OUTPUT_VELOCITY
#define OUTLINE_OUTPUT_VELOCITY 1
#endif
#if OUTLINE_OUTPUT_VELOCITY
[[vk::binding(8, 0)]] StructuredBuffer<RendererPreviousTransformData> previousTransforms;
#endif

struct OutlineVertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(3)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 texCoord : TEXCOORD0;
    [[vk::location(4)]] float2 texCoord1 : TEXCOORD1;
    [[vk::location(8)]] uint materialIndex : MATERIAL_INDEX;
    [[vk::location(9)]] float4 tangent : TANGENT;
};

struct OutlineVertexOutput
{
    float4 position : SV_Position;
    [[vk::location(2)]] nointerpolation uint instanceIndex;
#if OUTLINE_OUTPUT_VELOCITY
    [[vk::location(3)]] noperspective float2 currentNdc;
    [[vk::location(4)]] float4 previousClip;
#endif
};

[shader("vertex")]
OutlineVertexOutput vertexMain(OutlineVertexInput input, uint instanceIndex : SV_InstanceID,
                               uint baseInstance : SV_StartInstanceLocation)
{
    const uint visibleIndex = visibleInstanceIndices[baseInstance + instanceIndex];
    RendererInstanceData instance = visibleInstances[visibleIndex];
    const uint materialIndex = asuint(instance.positionMaterial.w) + input.materialIndex;
    const DisplacedVertex vertex = displaceMaterialVertex(
        materials[materialIndex], input.position, input.normal, input.tangent,
        input.texCoord, input.texCoord1);
    float4 q = instance.rotation;
    float3x3 rotationMatrix = float3x3(
        1.0 - 2.0 * (q.y * q.y + q.z * q.z), 2.0 * (q.x * q.y + q.z * q.w), 2.0 * (q.x * q.z - q.y * q.w),
        2.0 * (q.x * q.y - q.z * q.w), 1.0 - 2.0 * (q.x * q.x + q.z * q.z), 2.0 * (q.y * q.z + q.x * q.w),
        2.0 * (q.x * q.z + q.y * q.w), 2.0 * (q.y * q.z - q.x * q.w), 1.0 - 2.0 * (q.x * q.x + q.y * q.y));
    float4 worldPosition = float4(mul(vertex.position * instance.scaleBase.xyz, rotationMatrix) +
                                  instance.positionMaterial.xyz, 1.0);
    float4 clipPosition = mul(mul(worldPosition, frame.view), frame.projection);
    float3 viewNormal = normalize(mul(mul(vertex.normal / instance.scaleBase.xyz, rotationMatrix),
                                      (float3x3)frame.view));
    float2 screenDirection = normalize(viewNormal.xy);
    if (length(viewNormal.xy) < 0.001)
        screenDirection = float2(0.0, 1.0);
    clipPosition.xy += screenDirection * 0.012 * clipPosition.w;

    OutlineVertexOutput output;
    output.position = clipPosition;
    output.instanceIndex = instanceIndex;
#if OUTLINE_OUTPUT_VELOCITY
    RendererPreviousTransformData previous = previousTransforms[visibleIndex];
    float4 previousQ = previous.previousRotation;
    float3x3 previousRotation = float3x3(
        1.0 - 2.0 * (previousQ.y * previousQ.y + previousQ.z * previousQ.z), 2.0 * (previousQ.x * previousQ.y + previousQ.z * previousQ.w), 2.0 * (previousQ.x * previousQ.z - previousQ.y * previousQ.w),
        2.0 * (previousQ.x * previousQ.y - previousQ.z * previousQ.w), 1.0 - 2.0 * (previousQ.x * previousQ.x + previousQ.z * previousQ.z), 2.0 * (previousQ.y * previousQ.z + previousQ.x * previousQ.w),
        2.0 * (previousQ.x * previousQ.z + previousQ.y * previousQ.w), 2.0 * (previousQ.y * previousQ.z - previousQ.x * previousQ.w), 1.0 - 2.0 * (previousQ.x * previousQ.x + previousQ.y * previousQ.y));
    float4 previousClip = mul(mul(float4(mul(vertex.position * previous.previousScale.xyz, previousRotation) +
        previous.previousPosition.xyz, 1.0), frame.previousView), frame.previousProjection);
    float3 previousViewNormal = normalize(mul(mul(vertex.normal / previous.previousScale.xyz,
        previousRotation), (float3x3)frame.previousView));
    float2 previousScreenDirection = length(previousViewNormal.xy) < 0.001
        ? float2(0.0, 1.0) : normalize(previousViewNormal.xy);
    previousClip.xy += previousScreenDirection * 0.012 * previousClip.w;
    output.currentNdc = clipPosition.xy / clipPosition.w;
    output.previousClip = previousClip;
#endif
    return output;
}

struct OutlineFragmentInput
{
    [[vk::location(2)]] nointerpolation uint instanceIndex;
#if OUTLINE_OUTPUT_VELOCITY
    [[vk::location(3)]] noperspective float2 currentNdc;
    [[vk::location(4)]] float4 previousClip;
#endif
};

struct OutlineFragmentOutput
{
    [[vk::location(0)]] float4 color : SV_Target;
#if OUTLINE_OUTPUT_VELOCITY
    [[vk::location(1)]] float2 velocity : SV_Target1;
#endif
};

[shader("fragment")]
OutlineFragmentOutput fragmentMain(OutlineFragmentInput input)
{
    if (input.instanceIndex != frame.selectedInstance)
        discard;

    OutlineFragmentOutput output;
    output.color = 1.0;
#if OUTLINE_OUTPUT_VELOCITY
    output.velocity = (input.currentNdc - input.previousClip.xy / input.previousClip.w) * 0.5;
#endif
    return output;
}
