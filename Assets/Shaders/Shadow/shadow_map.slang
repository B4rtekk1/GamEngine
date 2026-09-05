import Common.SceneData;
import Common.GrassWind;

struct PushConstants
{
    float4x4 lightSpace;
};

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(5, 0)]] StructuredBuffer<RendererInstanceData> visibleGrassInstances;
[[vk::binding(6, 0)]] StructuredBuffer<uint> visibleGrassIndices;
[[vk::push_constant]] ConstantBuffer<PushConstants> pushConstants;

struct ShadowVertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(2)]] float2 texCoord : TEXCOORD0;
    [[vk::location(8)]] uint materialIndex : MATERIAL_INDEX;
};

struct ShadowVertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 texCoord;
    [[vk::location(1)]] nointerpolation uint materialIndex;
};

[shader("vertex")]
ShadowVertexOutput vertexMain(ShadowVertexInput input, uint instanceIndex : SV_InstanceID,
                              uint baseInstance : SV_StartInstanceLocation)
{
    RendererInstanceData instance = visibleGrassInstances[visibleGrassIndices[baseInstance + instanceIndex]];
    ShadowVertexOutput output;
    float3 localPosition = input.position;
    float normalizedHeight = clamp((input.position.y - instance.scaleBase.w) *
                                   instance.grassDeformation.w, 0.0, 1.0);
    float bendCurve = normalizedHeight * normalizedHeight * (3.0 - 2.0 * normalizedHeight);
    float bendWeight = bendCurve * instance.grassDeformation.z;
    float grassHeight = instance.grassDeformation.w > 0.0 ? 1.0 / instance.grassDeformation.w : 0.0;
    localPosition.xz += instance.grassDeformation.xy * bendWeight * grassHeight * 0.8;
    localPosition.y -= bendWeight * (0.25 + 0.30 * normalizedHeight) * grassHeight;
    float4 inverseInstanceRotation = float4(-instance.rotation.xyz, instance.rotation.w);
    localPosition += grassWindDisplacement(instance.positionMaterial.xz, inverseInstanceRotation,
                                           normalizedHeight, grassHeight,
                                           frame.windGustFrequencyTime.z,
                                           frame.windDirectionStrength,
                                           frame.windSourcePositionRange,
                                           frame.windGustFrequencyTime);
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
    return output;
}

[[vk::binding(2, 0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(8, 0)]] Sampler2D materialTextures[];

struct ShadowFragmentInput
{
    [[vk::location(0)]] float2 texCoord;
    [[vk::location(1)]] nointerpolation uint materialIndex;
};

[shader("fragment")]
void fragmentMain(ShadowFragmentInput input)
{
    MaterialData material = materials[input.materialIndex];
    float alpha = 1.0;
    if (material.textureIndices.x >= 0)
        alpha *= materialTextures[material.textureIndices.x].Sample(input.texCoord).a;
    // Must match forward_pbr: uncooked Quixel foliage commonly carries its
    // cutout mask separately rather than in base-color alpha.
    if (material.auxiliaryTextureIndices.y >= 0)
        alpha *= materialTextures[material.auxiliaryTextureIndices.y].Sample(input.texCoord).r;
    const uint alphaMode = uint((material.textureIndices.w >> 1) & 3);
    if (alphaMode == 1 && alpha < material.roughnessAmbientOcclusion.z)
        discard;
}
