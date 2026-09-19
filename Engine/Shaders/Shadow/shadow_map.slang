import Common.SceneData;

struct PushConstants
{
    float4x4 lightSpace;
};

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
struct ShadowInstanceTransform { float4 row0; float4 row1; float4 row2; };
[[vk::binding(5, 0)]] StructuredBuffer<ShadowInstanceTransform> shadowTransforms;
[[vk::binding(6, 0)]] StructuredBuffer<uint> shadowMaterialOffsets;
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
    const uint transformIndex = baseInstance + instanceIndex;
    const ShadowInstanceTransform transform = shadowTransforms[transformIndex];
    ShadowVertexOutput output;
    const float3 localPosition = input.position;
    float4 worldPosition = float4(dot(transform.row0, float4(localPosition, 1.0)),
                                  dot(transform.row1, float4(localPosition, 1.0)),
                                  dot(transform.row2, float4(localPosition, 1.0)), 1.0);
    output.position = mul(worldPosition, pushConstants.lightSpace);
    output.texCoord = input.texCoord;
    output.materialIndex = shadowMaterialOffsets[transformIndex] + input.materialIndex;
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
    const uint alphaMode = uint((material.textureIndices.w >> 1) & 3);
    // The overwhelmingly common opaque caster must be VS -> depth only in
    // practice: do not fetch albedo/opacity textures merely to discover that
    // it cannot discard. Alpha-tested foliage takes the slow path below.
    if (alphaMode == 0)
        return;
    float alpha = material.roughnessAmbientOcclusion.w;
    if (material.textureIndices.x >= 0)
        alpha *= materialTextures[NonUniformResourceIndex(material.textureIndices.x)].Sample(materialTextureUv(material, 0, input.texCoord, input.texCoord1)).a;
    // Must match forward_pbr: uncooked Quixel foliage commonly carries its
    // cutout mask separately rather than in base-color alpha.
    if (material.auxiliaryTextureIndices.y >= 0)
        alpha *= materialTextures[NonUniformResourceIndex(material.auxiliaryTextureIndices.y)].Sample(materialTextureUv(material, 4, input.texCoord, input.texCoord1)).r;
    // Match the forward foliage cutout policy. Quixel-style foliage may be
    // authored as BLEND despite using a binary opacity texture.
    const float cutoff = alphaMode == 1 ? material.roughnessAmbientOcclusion.z : 0.30;
    if ((alphaMode == 1 || alphaMode == 2) && alpha < cutoff)
        discard;
}
