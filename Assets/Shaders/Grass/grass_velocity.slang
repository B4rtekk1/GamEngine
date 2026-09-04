// Packed grass is static between frames; this pass still alpha-tests the same
// blades so TAA receives an explicit zero-motion sample instead of background
// velocity at grass pixels.
import Common.SceneData;
import Grass.GrassPackedData;

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(2, 0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(3, 0)]] Sampler2D materialTextures[16];
[[vk::binding(5, 0)]] StructuredBuffer<GrassPackedInstance> instances;
[[vk::binding(6, 0)]] StructuredBuffer<uint> visibleInstances;
[[vk::binding(7, 0)]] StructuredBuffer<GrassPackedCluster> clusters;

struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(2)]] float2 texCoord : TEXCOORD0;
    [[vk::location(8)]] uint materialIndex : MATERIAL_INDEX;
};
struct VertexOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 texCoord;
    [[vk::location(1)]] nointerpolation uint materialIndex;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input, uint instanceIndex : SV_InstanceID,
                        uint baseInstance : SV_StartInstanceLocation) {
    GrassPackedInstance instance = instances[visibleInstances[baseInstance + instanceIndex]];
    GrassPackedCluster cluster = clusters[instance.flags];
    float3 world = grassPackedWorldPosition(instance, cluster) +
                   grassRotateYaw(input.position * grassPackedScale(instance), grassPackedYaw(instance));
    VertexOutput output;
    output.position = mul(mul(float4(world, 1.0), frame.view), frame.projection);
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
    return float4(0.0, 0.0, 0.0, 1.0);
}
