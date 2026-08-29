import Common.SceneData;

struct PushConstants
{
    float4x4 lightSpace;
};

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::push_constant]] ConstantBuffer<PushConstants> pushConstants;

struct ShadowVertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(2)]] float2 texCoord : TEXCOORD0;
    [[vk::location(4)]] float4x4 instanceModel : INSTANCE_MODEL;
    [[vk::location(8)]] uint materialIndex : MATERIAL_INDEX;
    [[vk::location(10)]] float4 normalColumn0 : NORMAL_COLUMN0;
    [[vk::location(11)]] float4 normalColumn1 : NORMAL_COLUMN1;
    [[vk::location(13)]] float4 grassDeformation : GRASS_DEFORMATION;
};

struct ShadowVertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 texCoord;
    [[vk::location(1)]] nointerpolation uint materialIndex;
};

[shader("vertex")]
ShadowVertexOutput vertexMain(ShadowVertexInput input, uint instanceIndex : SV_InstanceID)
{
    ShadowVertexOutput output;
    float3 localPosition = input.position;
    float normalizedHeight = clamp((input.position.y - input.normalColumn0.w) *
                                   input.normalColumn1.w, 0.0, 1.0);
    float bendWeight = normalizedHeight * normalizedHeight * input.grassDeformation.z;
    float grassHeight = input.normalColumn1.w > 0.0 ? 1.0 / input.normalColumn1.w : 0.0;
    localPosition.xz += input.grassDeformation.xy * bendWeight * grassHeight * 0.8;
    localPosition.y -= bendWeight * normalizedHeight * grassHeight * 0.55;
    output.position = mul(mul(float4(localPosition, 1.0), input.instanceModel),
                          pushConstants.lightSpace);
    output.texCoord = input.texCoord;
    output.materialIndex = instanceIndex * frame.materialSlots + input.materialIndex;
    return output;
}

[[vk::binding(2, 0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(3, 0)]] Sampler2D materialTextures[16];

struct ShadowFragmentInput
{
    [[vk::location(0)]] float2 texCoord;
    [[vk::location(1)]] nointerpolation uint materialIndex;
};

[shader("fragment")]
void fragmentMain(ShadowFragmentInput input)
{
    MaterialData material = materials[input.materialIndex];
    if (material.textureIndices.x >= 0)
    {
        const float alpha = materialTextures[material.textureIndices.x].Sample(input.texCoord).a;
        const float cutoff = (material.textureIndices.w & 2) != 0
            ? material.roughnessAmbientOcclusion.z : 0.01;
        if (alpha < cutoff)
            discard;
    }
}
