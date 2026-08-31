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
    [[vk::location(4)]] float4 positionMaterial : INSTANCE_POSITION_MATERIAL;
    [[vk::location(5)]] float4 instanceRotation : INSTANCE_ROTATION;
    [[vk::location(6)]] float4 scaleBase : INSTANCE_SCALE_BASE;
    [[vk::location(8)]] uint materialIndex : MATERIAL_INDEX;
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
    float normalizedHeight = clamp((input.position.y - input.scaleBase.w) *
                                   input.grassDeformation.w, 0.0, 1.0);
    float bendCurve = normalizedHeight * normalizedHeight * (3.0 - 2.0 * normalizedHeight);
    float bendWeight = bendCurve * input.grassDeformation.z;
    float grassHeight = input.grassDeformation.w > 0.0 ? 1.0 / input.grassDeformation.w : 0.0;
    localPosition.xz += input.grassDeformation.xy * bendWeight * grassHeight * 0.8;
    localPosition.y -= bendWeight * (0.25 + 0.30 * normalizedHeight) * grassHeight;
    float4 q = input.instanceRotation;
    float3x3 rotationMatrix = float3x3(
        1.0 - 2.0 * (q.y * q.y + q.z * q.z), 2.0 * (q.x * q.y + q.z * q.w), 2.0 * (q.x * q.z - q.y * q.w),
        2.0 * (q.x * q.y - q.z * q.w), 1.0 - 2.0 * (q.x * q.x + q.z * q.z), 2.0 * (q.y * q.z + q.x * q.w),
        2.0 * (q.x * q.z + q.y * q.w), 2.0 * (q.y * q.z - q.x * q.w), 1.0 - 2.0 * (q.x * q.x + q.y * q.y));
    float4 worldPosition = float4(mul(localPosition * input.scaleBase.xyz, rotationMatrix) +
                                  input.positionMaterial.xyz, 1.0);
    output.position = mul(worldPosition, pushConstants.lightSpace);
    output.texCoord = input.texCoord;
    output.materialIndex = asuint(input.positionMaterial.w) + input.materialIndex;
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
