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
    output.position = mul(mul(float4(input.position, 1.0), input.instanceModel),
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
