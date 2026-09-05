// Dedicated alpha-tested grass shadow caster. Its descriptor contract matches
// grass_forward: bindings 5-7 are packed instances, visibility IDs, clusters.
import Common.SceneData;
import Grass.GrassPackedData;
import Grass.GrassDeformation;

struct PushConstants { float4x4 lightSpace; };
[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(2, 0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(8, 0)]] Sampler2D materialTextures[];
[[vk::binding(5, 0)]] StructuredBuffer<GrassPackedInstance> instances;
[[vk::binding(6, 0)]] StructuredBuffer<uint> visibleInstances;
[[vk::binding(3, 0)]] StructuredBuffer<GrassPackedCluster> clusters;
[[vk::binding(7, 0)]] StructuredBuffer<GrassPackedDeformation> deformations;
[[vk::push_constant]] ConstantBuffer<PushConstants> pushConstants;

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
                        uint baseInstance : SV_StartInstanceLocation)
{
    uint packedIndex = visibleInstances[baseInstance + instanceIndex];
    GrassPackedInstance instance = instances[packedIndex];
    GrassPackedCluster cluster = clusters[instance.flags];
    float3 world = grassPackedDeformedVertex(input.position, float3(0.0), instance, cluster,
                                              deformations[packedIndex], frame, false).position;
    VertexOutput output;
    output.position = mul(float4(world, 1.0), pushConstants.lightSpace);
    output.texCoord = input.texCoord;
    output.materialIndex = cluster.instanceRange.z + input.materialIndex;
    return output;
}

[shader("fragment")]
void fragmentMain(VertexOutput input)
{
    MaterialData material = materials[input.materialIndex];
    if (material.textureIndices.x >= 0 &&
        materialTextures[material.textureIndices.x].Sample(input.texCoord).a <
            max(material.roughnessAmbientOcclusion.z, 0.01)) discard;
}
