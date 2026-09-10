// Dedicated packed-grass forward path.  Binding 6 is updated each frame to
// the compacted main visibility stream before its counted indirect draw.
import Common.SceneData;
import Common.ShadowSampling;
import Grass.GrassPackedData;
import Grass.GrassDeformation;

#ifndef FORWARD_OUTPUT_VELOCITY
#define FORWARD_OUTPUT_VELOCITY 1
#endif

[[vk::binding(0, 0)]] Sampler2DShadow shadowMap;
[[vk::binding(9, 0)]] Sampler2D shadowDepthMap;
[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(2, 0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(10, 0)]] Sampler2D materialTextures[];
[[vk::binding(4, 0)]] StructuredBuffer<uint4> shadowPageTable;
[[vk::binding(5, 0)]] StructuredBuffer<GrassPackedInstance> instances;
[[vk::binding(6, 0)]] StructuredBuffer<uint> visibleInstances;
[[vk::binding(3, 0)]] StructuredBuffer<GrassPackedCluster> clusters;
[[vk::binding(7, 0)]] StructuredBuffer<GrassPackedDeformation> deformations;

struct VertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 color : COLOR0;
    [[vk::location(2)]] float2 texCoord : TEXCOORD0;
    [[vk::location(3)]] float3 normal : NORMAL;
    [[vk::location(8)]] uint materialIndex : MATERIAL_INDEX;
};

struct VertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 color;
    [[vk::location(1)]] float3 worldPosition;
    [[vk::location(2)]] float3 normal;
    [[vk::location(3)]] nointerpolation uint materialIndex;
    [[vk::location(4)]] float2 texCoord;
    [[vk::location(5)]] noperspective float2 currentNdc;
    [[vk::location(6)]] float4 previousClip;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input, uint instanceIndex : SV_InstanceID,
                        uint baseInstance : SV_StartInstanceLocation)
{
    GrassPackedInstance instance = instances[visibleInstances[baseInstance + instanceIndex]];
    GrassPackedCluster cluster = clusters[instance.flags];
    uint packedIndex = visibleInstances[baseInstance + instanceIndex];
    GrassDeformedVertex deformed = grassPackedDeformedVertex(input.position, input.normal, instance, cluster,
                                                              deformations[packedIndex], frame, false);
    float3 worldPosition = deformed.position;
    VertexOutput output;
    float4 currentClip = mul(mul(float4(worldPosition, 1.0), frame.view), frame.projection);
    float3 previousWorld = grassPackedDeformedVertex(input.position, input.normal, instance, cluster,
                                                      deformations[packedIndex], frame, true).position;
    output.position = currentClip;
    output.color = input.color;
    output.worldPosition = worldPosition;
    output.normal = deformed.normal;
    output.materialIndex = cluster.instanceRange.z + input.materialIndex;
    output.texCoord = input.texCoord;
    output.currentNdc = currentClip.xy / currentClip.w;
    output.previousClip = mul(mul(float4(previousWorld, 1.0), frame.previousView), frame.previousProjection);
    return output;
}

struct FragmentInput
{
    [[vk::location(0)]] float3 color;
    [[vk::location(1)]] float3 worldPosition;
    [[vk::location(2)]] float3 normal;
    [[vk::location(3)]] nointerpolation uint materialIndex;
    [[vk::location(4)]] float2 texCoord;
    [[vk::location(5)]] noperspective float2 currentNdc;
    [[vk::location(6)]] float4 previousClip;
    bool isFrontFace : SV_IsFrontFace;
};

struct GrassFragmentOutput {
    [[vk::location(0)]] float4 color : SV_Target;
#if FORWARD_OUTPUT_VELOCITY
    [[vk::location(1)]] float2 velocity : SV_Target1;
#endif
};

[shader("fragment")]
GrassFragmentOutput fragmentMain(FragmentInput input)
{
    MaterialData material = materials[input.materialIndex];
    float4 albedo = float4(input.color * material.baseColorMetallic.rgb, 1.0);
    if (material.textureIndices.x >= 0) albedo *= materialTextures[NonUniformResourceIndex(material.textureIndices.x)].Sample(input.texCoord);
    if (albedo.a < max(material.roughnessAmbientOcclusion.z, 0.01)) discard;
    float3 normal = normalize(input.isFrontFace ? input.normal : -input.normal);
    float3 lightDirection = normalize(-frame.lightDirectionIntensity.xyz);
    float diffuse = saturate(dot(normal, lightDirection) * 0.5 + 0.5);
    ShadowSampleDebug shadowDebug = {0.0, 0.0, 0u, uint2(0, 0)};
    float shadow = frame.shadowEnabled != 0 ? sampleVirtualShadow(
        shadowMap, shadowDepthMap, input.worldPosition, normal, lightDirection, frame.shadowClipMatrices, shadowPageTable,
        frame.shadowQuality, shadowDebug) : 0.0;
    GrassFragmentOutput output;
    output.color = frame.shadowDebugView != SHADOW_DEBUG_OFF
        ? float4(shadowDebugColor(frame.shadowDebugView, shadowDebug), 1.0)
        : float4(albedo.rgb * (0.20 + diffuse * frame.lightDirectionIntensity.w * (1.0 - shadow) * frame.lightColor.rgb), 1.0);
#if FORWARD_OUTPUT_VELOCITY
    output.velocity = (input.currentNdc - input.previousClip.xy / input.previousClip.w) * 0.5;
#endif
    return output;
}
