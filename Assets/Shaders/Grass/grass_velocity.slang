import Common.SceneData;
import Grass.GrassPackedData;
import Grass.GrassDeformation;

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(2, 0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(8, 0)]] Sampler2D materialTextures[];
[[vk::binding(5, 0)]] StructuredBuffer<GrassPackedInstance> instances;
[[vk::binding(6, 0)]] StructuredBuffer<uint> visibleInstances;
[[vk::binding(3, 0)]] StructuredBuffer<GrassPackedCluster> clusters;
[[vk::binding(7, 0)]] StructuredBuffer<GrassPackedDeformation> deformations;

struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(2)]] float2 texCoord : TEXCOORD0;
    [[vk::location(8)]] uint materialIndex : MATERIAL_INDEX;
};
struct VertexOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] noperspective float2 currentNdc;
    [[vk::location(1)]] float4 previousClip;
    [[vk::location(2)]] float2 texCoord;
    [[vk::location(3)]] nointerpolation uint materialIndex;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input, uint instanceIndex : SV_InstanceID,
                        uint baseInstance : SV_StartInstanceLocation) {
    uint packedIndex = visibleInstances[baseInstance + instanceIndex];
    GrassPackedInstance instance = instances[packedIndex];
    GrassPackedCluster cluster = clusters[instance.flags];
    float3 world = grassPackedDeformedVertex(input.position, float3(0.0), instance, cluster,
                                              deformations[packedIndex], frame, false).position;
    float3 previousWorld = grassPackedDeformedVertex(input.position, float3(0.0), instance, cluster,
                                                      deformations[packedIndex], frame, true).position;
    VertexOutput output;
    float4 currentClip = mul(mul(float4(world, 1.0), frame.view), frame.projection);
    output.position = currentClip;
    output.currentNdc = currentClip.xy / currentClip.w;
    output.previousClip = mul(mul(float4(previousWorld, 1.0), frame.previousView), frame.previousProjection);
    output.texCoord = input.texCoord;
    output.materialIndex = cluster.instanceRange.z + input.materialIndex;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    MaterialData material = materials[input.materialIndex];
    if (material.textureIndices.x >= 0 &&
        materialTextures[material.textureIndices.x].Sample(input.texCoord).a <= 0.01)
        discard;
    float2 currentUv = input.currentNdc * 0.5 + 0.5;
    float2 previousUv = (input.previousClip.xy / input.previousClip.w) * 0.5 + 0.5;
    return float4(currentUv - previousUv, 0.0, 1.0);
}
