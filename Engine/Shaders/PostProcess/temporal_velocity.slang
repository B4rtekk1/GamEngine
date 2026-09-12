import Common.SceneData;

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(2, 0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(13, 0)]] Sampler2D materialTextures[];
[[vk::binding(5, 0)]] StructuredBuffer<RendererInstanceData> visibleInstances;
[[vk::binding(6, 0)]] StructuredBuffer<uint> visibleInstanceIndices;
[[vk::binding(8, 0)]] StructuredBuffer<RendererPreviousTransformData> previousTransforms;

struct VertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(2)]] float2 texCoord : TEXCOORD0;
    [[vk::location(4)]] float2 texCoord1 : TEXCOORD1;
    [[vk::location(8)]] uint materialIndex : MATERIAL_INDEX;
};

float3 rotateByQuaternion(float3 value, float4 q)
{
    return value + 2.0 * cross(q.xyz, cross(q.xyz, value) + q.w * value);
}

struct VertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] noperspective float2 currentNdc;
    // Keep the previous clip position intact.  Perspective interpolation in
    // the current triangle is required before the previous divide.
    [[vk::location(1)]] float4 previousClip;
    [[vk::location(2)]] float2 texCoord;
    [[vk::location(3)]] nointerpolation uint materialIndex;
    [[vk::location(4)]] float2 texCoord1;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input, uint instanceIndex : SV_InstanceID,
                        uint baseInstance : SV_StartInstanceLocation)
{
    const uint visibleIndex = visibleInstanceIndices[baseInstance + instanceIndex];
    RendererInstanceData instance = visibleInstances[visibleIndex];
    RendererPreviousTransformData previousTransform = previousTransforms[visibleIndex];
    const float3 localPosition = input.position;
    const float3 previousLocalPosition = input.position;

    float4 currentWorld = float4(rotateByQuaternion(localPosition * instance.scaleBase.xyz,
                                                    instance.rotation) + instance.positionMaterial.xyz, 1.0);
    float4 previousWorld = float4(rotateByQuaternion(previousLocalPosition * previousTransform.previousScale.xyz,
                                                     previousTransform.previousRotation) + previousTransform.previousPosition.xyz, 1.0);

    VertexOutput output;
    float4 currentClip = mul(mul(currentWorld, frame.view), frame.projection);
    float4 previousClip = mul(mul(previousWorld, frame.previousView), frame.previousProjection);
    output.position = currentClip;
    output.currentNdc = currentClip.xy / currentClip.w;
    output.previousClip = previousClip;
    output.texCoord = input.texCoord;
    output.texCoord1 = input.texCoord1;
    output.materialIndex = asuint(instance.positionMaterial.w) + input.materialIndex;
    return output;
}

struct FragmentInput
{
    [[vk::location(0)]] noperspective float2 currentNdc;
    [[vk::location(1)]] float4 previousClip;
    [[vk::location(2)]] float2 texCoord;
    [[vk::location(3)]] nointerpolation uint materialIndex;
    [[vk::location(4)]] float2 texCoord1;
};

struct FragmentOutput
{
    [[vk::location(0)]] float4 velocity : SV_Target;
};

[shader("fragment")]
FragmentOutput fragmentMain(FragmentInput input)
{
    MaterialData material = materials[input.materialIndex];
    if (material.textureIndices.x >= 0 || material.auxiliaryTextureIndices.y >= 0)
    {
        float alpha = material.roughnessAmbientOcclusion.w;
        if (material.textureIndices.x >= 0)
            alpha *= materialTextures[NonUniformResourceIndex(material.textureIndices.x)].Sample(materialTextureUv(material, 0, input.texCoord, input.texCoord1)).a;
        if (material.auxiliaryTextureIndices.y >= 0)
            alpha *= materialTextures[NonUniformResourceIndex(material.auxiliaryTextureIndices.y)].Sample(materialTextureUv(material, 4, input.texCoord, input.texCoord1)).r;
        uint alphaMode = uint((material.textureIndices.w >> 1) & 3);
        const float cutoff = alphaMode == 1 ? material.roughnessAmbientOcclusion.z : 0.30;
        if ((alphaMode == 1 || alphaMode == 2) && alpha < cutoff)
            discard;
    }
    float2 currentUv = input.currentNdc * 0.5 + 0.5;
    float2 previousUv = (input.previousClip.xy / input.previousClip.w) * 0.5 + 0.5;
    FragmentOutput output;
    output.velocity = float4(currentUv - previousUv, 0.0, 1.0);
    return output;
}
