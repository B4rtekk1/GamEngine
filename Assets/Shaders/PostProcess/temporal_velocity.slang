import Common.SceneData;

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(2, 0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(3, 0)]] Sampler2D materialTextures[16];

struct VertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(2)]] float2 texCoord : TEXCOORD0;
    [[vk::location(4)]] float4 positionMaterial : INSTANCE_POSITION_MATERIAL;
    [[vk::location(5)]] float4 rotation : INSTANCE_ROTATION;
    [[vk::location(6)]] float4 scaleBase : INSTANCE_SCALE_BASE;
    [[vk::location(13)]] float4 grassDeformation : GRASS_DEFORMATION;
    [[vk::location(8)]] uint materialIndex : MATERIAL_INDEX;
    [[vk::location(14)]] float4 previousPosition : PREVIOUS_POSITION;
    [[vk::location(15)]] float4 previousRotation : PREVIOUS_ROTATION;
    [[vk::location(16)]] float4 previousScale : PREVIOUS_SCALE;
    [[vk::location(17)]] float4 previousGrassDeformation : PREVIOUS_GRASS_DEFORMATION;
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
VertexOutput vertexMain(VertexInput input)
{
    float3 localPosition = input.position;
    float normalizedHeight = clamp((input.position.y - input.scaleBase.w) *
                                   input.grassDeformation.w, 0.0, 1.0);
    float bendWeight = normalizedHeight * normalizedHeight * input.grassDeformation.z;
    float grassHeight = input.grassDeformation.w > 0.0 ? 1.0 / input.grassDeformation.w : 0.0;
    localPosition.xz += input.grassDeformation.xy * bendWeight * grassHeight * 0.8;
    localPosition.y -= bendWeight * normalizedHeight * grassHeight * 0.55;

    float3 previousLocalPosition = input.position;
    float previousNormalizedHeight = clamp((input.position.y - input.previousScale.w) *
                                           input.previousGrassDeformation.w, 0.0, 1.0);
    float previousBendWeight = previousNormalizedHeight * previousNormalizedHeight *
                               input.previousGrassDeformation.z;
    float previousGrassHeight = input.previousGrassDeformation.w > 0.0
        ? 1.0 / input.previousGrassDeformation.w : 0.0;
    previousLocalPosition.xz += input.previousGrassDeformation.xy * previousBendWeight *
                                previousGrassHeight * 0.8;
    previousLocalPosition.y -= previousBendWeight * previousNormalizedHeight *
                               previousGrassHeight * 0.55;

    float4 currentWorld = float4(rotateByQuaternion(localPosition * input.scaleBase.xyz,
                                                    input.rotation) + input.positionMaterial.xyz, 1.0);
    float4 previousWorld = float4(rotateByQuaternion(previousLocalPosition * input.previousScale.xyz,
                                                     input.previousRotation) + input.previousPosition.xyz, 1.0);

    VertexOutput output;
    float4 currentClip = mul(mul(currentWorld, frame.view), frame.projection);
    float4 previousClip = mul(mul(previousWorld, frame.previousView), frame.previousProjection);
    output.position = currentClip;
    output.currentNdc = currentClip.xy / currentClip.w;
    output.previousClip = previousClip;
    output.texCoord = input.texCoord;
    output.materialIndex = asuint(input.positionMaterial.w) + input.materialIndex;
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
        bool alphaBlend = (material.textureIndices.w & 2) != 0;
        if ((alphaBlend && alpha < material.roughnessAmbientOcclusion.z) ||
            (!alphaBlend && alpha <= 0.01))
            discard;
    }
    float2 currentUv = input.currentNdc * 0.5 + 0.5;
    float2 previousUv = (input.previousClip.xy / input.previousClip.w) * 0.5 + 0.5;
    FragmentOutput output;
    output.velocity = float4(currentUv - previousUv, 0.0, 1.0);
    return output;
}
