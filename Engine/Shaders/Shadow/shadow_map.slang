import Common.SceneData;

struct PushConstants
{
    float4x4 lightSpace;
};

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(5, 0)]] StructuredBuffer<RendererInstanceData> visibleInstances;
[[vk::binding(6, 0)]] StructuredBuffer<uint> visibleInstanceIndices;
[[vk::push_constant]] ConstantBuffer<PushConstants> pushConstants;

struct ShadowVertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(2)]] float2 texCoord : TEXCOORD0;
    [[vk::location(4)]] float2 texCoord1 : TEXCOORD1;
    [[vk::location(8)]] uint materialIndex : MATERIAL_INDEX;
};

struct ShadowVertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 texCoord;
    [[vk::location(1)]] nointerpolation uint materialIndex;
    [[vk::location(2)]] float2 texCoord1;
};

[shader("vertex")]
ShadowVertexOutput vertexMain(ShadowVertexInput input, uint instanceIndex : SV_InstanceID,
                              uint baseInstance : SV_StartInstanceLocation)
{
    RendererInstanceData instance = visibleInstances[visibleInstanceIndices[baseInstance + instanceIndex]];
    ShadowVertexOutput output;
    const float3 localPosition = input.position;
    float4 q = instance.rotation;
    float3x3 rotationMatrix = float3x3(
        1.0 - 2.0 * (q.y * q.y + q.z * q.z), 2.0 * (q.x * q.y + q.z * q.w), 2.0 * (q.x * q.z - q.y * q.w),
        2.0 * (q.x * q.y - q.z * q.w), 1.0 - 2.0 * (q.x * q.x + q.z * q.z), 2.0 * (q.y * q.z + q.x * q.w),
        2.0 * (q.x * q.z + q.y * q.w), 2.0 * (q.y * q.z - q.x * q.w), 1.0 - 2.0 * (q.x * q.x + q.y * q.y));
    float4 worldPosition = float4(mul(localPosition * instance.scaleBase.xyz, rotationMatrix) +
                                  instance.positionMaterial.xyz, 1.0);
    output.position = mul(worldPosition, pushConstants.lightSpace);
    output.texCoord = input.texCoord;
    output.materialIndex = asuint(instance.positionMaterial.w) + input.materialIndex;
    output.texCoord1 = input.texCoord1;
    return output;
}

[[vk::binding(2, 0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(13, 0)]] Sampler2D materialTextures[];

struct ShadowFragmentInput
{
    [[vk::location(0)]] float2 texCoord;
    [[vk::location(1)]] nointerpolation uint materialIndex;
    [[vk::location(2)]] float2 texCoord1;
};

[shader("fragment")]
void fragmentMain(ShadowFragmentInput input)
{
    MaterialData material = materials[input.materialIndex];
    float alpha = material.roughnessAmbientOcclusion.w;
    if (material.textureIndices.x >= 0)
        alpha *= materialTextures[NonUniformResourceIndex(material.textureIndices.x)].Sample(materialTextureUv(material, 0, input.texCoord, input.texCoord1)).a;
    // Must match forward_pbr: uncooked Quixel foliage commonly carries its
    // cutout mask separately rather than in base-color alpha.
    if (material.auxiliaryTextureIndices.y >= 0)
        alpha *= materialTextures[NonUniformResourceIndex(material.auxiliaryTextureIndices.y)].Sample(materialTextureUv(material, 4, input.texCoord, input.texCoord1)).r;
    const uint alphaMode = uint((material.textureIndices.w >> 1) & 3);
    // Match the forward foliage cutout policy. Quixel-style foliage may be
    // authored as BLEND despite using a binary opacity texture.
    const float cutoff = alphaMode == 1 ? material.roughnessAmbientOcclusion.z : 0.30;
    if ((alphaMode == 1 || alphaMode == 2) && alpha < cutoff)
        discard;
}
