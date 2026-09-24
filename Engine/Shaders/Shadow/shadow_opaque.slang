// Opaque one-sided VSM casters: vertex-to-depth only.  Alpha-tested and
// foliage casters remain in shadow_map.slang, where discard is required.
import Common.SceneData;
import Common.Displacement;

struct PushConstants { float4x4 lightSpace; };
struct ShadowInstanceTransform { float4 row0; float4 row1; float4 row2; };
[[vk::binding(5, 0)]] StructuredBuffer<ShadowInstanceTransform> shadowTransforms;
[[vk::binding(2, 0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(6, 0)]] StructuredBuffer<uint> shadowMaterialOffsets;
[[vk::push_constant]] ConstantBuffer<PushConstants> pushConstants;

struct ShadowVertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(2)]] float2 texCoord : TEXCOORD0;
    [[vk::location(3)]] float3 normal : NORMAL;
    [[vk::location(4)]] float2 texCoord1 : TEXCOORD1;
    [[vk::location(8)]] uint materialIndex : MATERIAL_INDEX;
};
struct ShadowVertexOutput { float4 position : SV_Position; };

[shader("vertex")]
ShadowVertexOutput vertexMain(ShadowVertexInput input, uint instanceIndex : SV_InstanceID,
                              uint baseInstance : SV_StartInstanceLocation)
{
    const uint transformIndex = baseInstance + instanceIndex;
    const ShadowInstanceTransform transform = shadowTransforms[transformIndex];
    const uint materialIndex = shadowMaterialOffsets[transformIndex] + input.materialIndex;
    MaterialData material = materials[materialIndex];
    float3 localPosition = input.position;
    localPosition = displaceMaterialPosition(material, localPosition, input.normal,
                                             input.texCoord, input.texCoord1);
    ShadowVertexOutput output;
    const float3 world = float3(dot(transform.row0, float4(localPosition, 1.0)),
                                dot(transform.row1, float4(localPosition, 1.0)),
                                dot(transform.row2, float4(localPosition, 1.0)));
    output.position = mul(float4(world, 1.0), pushConstants.lightSpace);
    return output;
}
