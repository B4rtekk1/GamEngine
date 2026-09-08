import Common.SceneData;

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(5, 0)]] StructuredBuffer<RendererInstanceData> visibleGrassInstances;
[[vk::binding(6, 0)]] StructuredBuffer<uint> visibleGrassIndices;

struct OutlineVertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(3)]] float3 normal : NORMAL;
};

struct OutlineVertexOutput
{
    float4 position : SV_Position;
    [[vk::location(2)]] nointerpolation uint instanceIndex;
};

[shader("vertex")]
OutlineVertexOutput vertexMain(OutlineVertexInput input, uint instanceIndex : SV_InstanceID,
                               uint baseInstance : SV_StartInstanceLocation)
{
    RendererInstanceData instance = visibleGrassInstances[visibleGrassIndices[baseInstance + instanceIndex]];
    float4 q = instance.rotation;
    float3x3 rotationMatrix = float3x3(
        1.0 - 2.0 * (q.y * q.y + q.z * q.z), 2.0 * (q.x * q.y + q.z * q.w), 2.0 * (q.x * q.z - q.y * q.w),
        2.0 * (q.x * q.y - q.z * q.w), 1.0 - 2.0 * (q.x * q.x + q.z * q.z), 2.0 * (q.y * q.z + q.x * q.w),
        2.0 * (q.x * q.z + q.y * q.w), 2.0 * (q.y * q.z - q.x * q.w), 1.0 - 2.0 * (q.x * q.x + q.y * q.y));
    float4 worldPosition = float4(mul(input.position * instance.scaleBase.xyz, rotationMatrix) +
                                  instance.positionMaterial.xyz, 1.0);
    float4 clipPosition = mul(mul(worldPosition, frame.view), frame.projection);
    float3 viewNormal = normalize(mul(mul(input.normal / instance.scaleBase.xyz, rotationMatrix),
                                      (float3x3)frame.view));
    float2 screenDirection = normalize(viewNormal.xy);
    if (length(viewNormal.xy) < 0.001)
        screenDirection = float2(0.0, 1.0);
    clipPosition.xy += screenDirection * 0.012 * clipPosition.w;

    OutlineVertexOutput output;
    output.position = clipPosition;
    output.instanceIndex = instanceIndex;
    return output;
}

struct OutlineFragmentInput
{
    [[vk::location(2)]] nointerpolation uint instanceIndex;
};

struct OutlineFragmentOutput
{
    [[vk::location(0)]] float4 color : SV_Target;
};

[shader("fragment")]
OutlineFragmentOutput fragmentMain(OutlineFragmentInput input)
{
    if (input.instanceIndex != frame.selectedInstance)
        discard;

    OutlineFragmentOutput output;
    output.color = 1.0;
    return output;
}
