import Common.SceneData;
import Common.GrassWind;

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(2, 0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(8, 0)]] Sampler2D materialTextures[];
[[vk::binding(5, 0)]] StructuredBuffer<RendererInstanceData> visibleGrassInstances;
[[vk::binding(6, 0)]] StructuredBuffer<uint> visibleGrassIndices;

struct VertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(2)]] float2 texCoord : TEXCOORD0;
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
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input, uint instanceIndex : SV_InstanceID,
                        uint baseInstance : SV_StartInstanceLocation)
{
    RendererInstanceData instance = visibleGrassInstances[visibleGrassIndices[baseInstance + instanceIndex]];
    float3 localPosition = input.position;
    float normalizedHeight = clamp((input.position.y - instance.scaleBase.w) *
                                   instance.grassDeformation.w, 0.0, 1.0);
    float bendCurve = normalizedHeight * normalizedHeight * (3.0 - 2.0 * normalizedHeight);
    float bendWeight = bendCurve * instance.grassDeformation.z;
    float grassHeight = instance.grassDeformation.w > 0.0 ? 1.0 / instance.grassDeformation.w : 0.0;
    localPosition.xz += instance.grassDeformation.xy * bendWeight * grassHeight * 0.8;
    localPosition.y -= bendWeight * (0.25 + 0.30 * normalizedHeight) * grassHeight;
    float4 inverseRotation = float4(-instance.rotation.xyz, instance.rotation.w);
    localPosition += grassWindDisplacement(instance.positionMaterial.xz, inverseRotation,
                                           normalizedHeight, grassHeight,
                                           frame.windGustFrequencyTime.z,
                                           frame.windDirectionStrength,
                                           frame.windSourcePositionRange,
                                           frame.windGustFrequencyTime);

    float3 previousLocalPosition = input.position;
    float previousNormalizedHeight = clamp((input.position.y - instance.previousScale.w) *
                                           instance.previousGrassDeformation.w, 0.0, 1.0);
    float previousBendCurve = previousNormalizedHeight * previousNormalizedHeight *
                              (3.0 - 2.0 * previousNormalizedHeight);
    float previousBendWeight = previousBendCurve * instance.previousGrassDeformation.z;
    float previousGrassHeight = instance.previousGrassDeformation.w > 0.0
        ? 1.0 / instance.previousGrassDeformation.w : 0.0;
    previousLocalPosition.xz += instance.previousGrassDeformation.xy * previousBendWeight *
                                previousGrassHeight * 0.8;
    previousLocalPosition.y -= previousBendWeight * (0.25 + 0.30 * previousNormalizedHeight) *
                               previousGrassHeight;
    float4 inversePreviousRotation = float4(-instance.previousRotation.xyz, instance.previousRotation.w);
    previousLocalPosition += grassWindDisplacement(instance.previousPosition.xz, inversePreviousRotation,
                                                   previousNormalizedHeight, previousGrassHeight,
                                                   frame.windGustFrequencyTime.w,
                                                   frame.windDirectionStrength,
                                                   frame.windSourcePositionRange,
                                                   frame.windGustFrequencyTime);

    float4 currentWorld = float4(rotateByQuaternion(localPosition * instance.scaleBase.xyz,
                                                    instance.rotation) + instance.positionMaterial.xyz, 1.0);
    float4 previousWorld = float4(rotateByQuaternion(previousLocalPosition * instance.previousScale.xyz,
                                                     instance.previousRotation) + instance.previousPosition.xyz, 1.0);

    VertexOutput output;
    float4 currentClip = mul(mul(currentWorld, frame.view), frame.projection);
    float4 previousClip = mul(mul(previousWorld, frame.previousView), frame.previousProjection);
    output.position = currentClip;
    output.currentNdc = currentClip.xy / currentClip.w;
    output.previousClip = previousClip;
    output.texCoord = input.texCoord;
    output.materialIndex = asuint(instance.positionMaterial.w) + input.materialIndex;
    return output;
}

struct FragmentInput
{
    [[vk::location(0)]] noperspective float2 currentNdc;
    [[vk::location(1)]] float4 previousClip;
    [[vk::location(2)]] float2 texCoord;
    [[vk::location(3)]] nointerpolation uint materialIndex;
};

struct FragmentOutput
{
    [[vk::location(0)]] float4 velocity : SV_Target;
};

[shader("fragment")]
FragmentOutput fragmentMain(FragmentInput input)
{
    MaterialData material = materials[input.materialIndex];
    if (material.textureIndices.x >= 0)
    {
        float alpha = materialTextures[material.textureIndices.x].Sample(input.texCoord).a;
        uint alphaMode = uint((material.textureIndices.w >> 1) & 3);
        if (alphaMode == 1 && alpha < material.roughnessAmbientOcclusion.z)
            discard;
    }
    float2 currentUv = input.currentNdc * 0.5 + 0.5;
    float2 previousUv = (input.previousClip.xy / input.previousClip.w) * 0.5 + 0.5;
    FragmentOutput output;
    output.velocity = float4(currentUv - previousUv, 0.0, 1.0);
    return output;
}
