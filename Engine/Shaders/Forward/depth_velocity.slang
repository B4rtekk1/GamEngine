// Minimal opaque depth/velocity prepass.  Keep this deliberately independent
// from forward_pbr: depth must not pay for BRDF, IBL, clustered lights, GTAO
// or virtual-shadow projection a second time.
import Common.SceneData;
import Common.Displacement;

#ifndef DEPTH_OUTPUT_VELOCITY
#define DEPTH_OUTPUT_VELOCITY 1
#endif

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(2, 0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(5, 0)]] StructuredBuffer<RendererInstanceData> visibleInstances;
[[vk::binding(6, 0)]] StructuredBuffer<uint> visibleInstanceIndices;
#if DEPTH_OUTPUT_VELOCITY
[[vk::binding(8, 0)]] StructuredBuffer<RendererPreviousTransformData> previousTransforms;
#endif

struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(3)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 texCoord : TEXCOORD0;
    [[vk::location(4)]] float2 texCoord1 : TEXCOORD1;
    [[vk::location(8)]] uint materialIndex : MATERIAL_INDEX;
    [[vk::location(9)]] float4 tangent : TANGENT;
};
struct VertexOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 texCoord;
    [[vk::location(1)]] float2 texCoord1;
    [[vk::location(2)]] nointerpolation uint materialIndex;
    [[vk::location(5)]] float3 viewNormal;
#if DEPTH_OUTPUT_VELOCITY
    [[vk::location(3)]] noperspective float2 currentNdc;
    [[vk::location(4)]] float4 previousClip;
#endif
};

float3x3 rotation(float4 q) {
    return float3x3(
        1.0 - 2.0 * (q.y*q.y + q.z*q.z), 2.0 * (q.x*q.y + q.z*q.w), 2.0 * (q.x*q.z - q.y*q.w),
        2.0 * (q.x*q.y - q.z*q.w), 1.0 - 2.0 * (q.x*q.x + q.z*q.z), 2.0 * (q.y*q.z + q.x*q.w),
        2.0 * (q.x*q.z + q.y*q.w), 2.0 * (q.y*q.z - q.x*q.w), 1.0 - 2.0 * (q.x*q.x + q.y*q.y));
}

[shader("vertex")]
VertexOutput vertexMain(VertexInput input, uint instanceIndex : SV_InstanceID,
                        uint baseInstance : SV_StartInstanceLocation) {
    uint visibleIndex = visibleInstanceIndices[baseInstance + instanceIndex];
    RendererInstanceData instance = visibleInstances[visibleIndex];
    uint materialIndex = asuint(instance.positionMaterial.w) + input.materialIndex;
    MaterialData material = materials[materialIndex];
    const DisplacedVertex displaced = displaceMaterialVertex(
        material, input.position, input.normal, input.tangent,
        input.texCoord, input.texCoord1);
    const float3 localPosition = displaced.position;
    float3 world = mul(localPosition * instance.scaleBase.xyz, rotation(instance.rotation)) + instance.positionMaterial.xyz;
    VertexOutput output;
    float4 currentClip = mul(mul(float4(world, 1.0), frame.view), frame.projection);
    output.position = currentClip;
    output.texCoord = input.texCoord;
    output.texCoord1 = input.texCoord1;
    output.materialIndex = materialIndex;
    float3 worldNormal = normalize(mul(displaced.normal / instance.scaleBase.xyz, rotation(instance.rotation)));
    output.viewNormal = normalize(mul(float4(worldNormal, 0.0), frame.view).xyz);
#if DEPTH_OUTPUT_VELOCITY
    RendererPreviousTransformData previous = previousTransforms[visibleIndex];
    float3 previousWorld = mul(localPosition * previous.previousScale.xyz, rotation(previous.previousRotation)) + previous.previousPosition.xyz;
    output.currentNdc = currentClip.xy / currentClip.w;
    output.previousClip = mul(mul(float4(previousWorld, 1.0), frame.previousView), frame.previousProjection);
#endif
    return output;
}

void alphaTest(VertexOutput input) {
    MaterialData material = materials[input.materialIndex];
    const uint alphaMode = uint((material.textureIndices.w >> 1) & 3);
    // Opaque materials never touch textures in this pass.
    if (alphaMode == 0) return;
    float alpha = material.roughnessAmbientOcclusion.w;
    if (material.textureIndices.x >= 0)
        alpha *= materialTextures[NonUniformResourceIndex(material.textureIndices.x)].Sample(
            materialTextureUv(material, 0, input.texCoord, input.texCoord1)).a;
    if (material.auxiliaryTextureIndices.y >= 0)
        alpha *= materialTextures[NonUniformResourceIndex(material.auxiliaryTextureIndices.y)].Sample(
            materialTextureUv(material, 4, input.texCoord, input.texCoord1)).r;
    const float cutoff = alphaMode == 1 ? material.roughnessAmbientOcclusion.z : 0.30;
    if (alpha < cutoff) discard;
}

#if DEPTH_OUTPUT_VELOCITY
float2 encodeOct(float3 n) { n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-5); if(n.z < 0) n.xy = (1-abs(n.yx))*float2(n.x>=0?1:-1,n.y>=0?1:-1); return n.xy; }
struct FragmentOutput { [[vk::location(1)]] float2 velocity : SV_Target1; [[vk::location(2)]] float2 viewNormal : SV_Target2; };
[shader("fragment")]
FragmentOutput fragmentMain(VertexOutput input, bool frontFace : SV_IsFrontFace) {
    alphaTest(input);
    FragmentOutput output;
    output.velocity = (input.currentNdc - input.previousClip.xy / input.previousClip.w) * 0.5;
    float3 viewNormal = normalize(input.viewNormal);
    MaterialData material = materials[input.materialIndex];
    if ((material.textureIndices.w & 1) != 0 && !frontFace) viewNormal = -viewNormal;
    output.viewNormal = encodeOct(viewNormal);
    return output;
}
#else
[shader("fragment")]
void fragmentMain(VertexOutput input) { alphaTest(input); }
#endif
