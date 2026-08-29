import Common.SceneData;

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;

struct OutlineVertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(3)]] float3 normal : NORMAL;
    [[vk::location(4)]] float4x4 instanceModel : INSTANCE_MODEL;
};

struct OutlineVertexOutput
{
    float4 position : SV_Position;
    [[vk::location(2)]] nointerpolation uint instanceIndex;
};

[shader("vertex")]
OutlineVertexOutput vertexMain(OutlineVertexInput input, uint instanceIndex : SV_InstanceID)
{
    float4 worldPosition = mul(float4(input.position, 1.0), input.instanceModel);
    float4 clipPosition = mul(mul(worldPosition, frame.view), frame.projection);
    float3 viewNormal = normalize(mul(input.normal,
        (float3x3)mul(input.instanceModel, frame.view)));
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
