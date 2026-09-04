// Dedicated foliage forward path. It is compiled now and will be bound by
// GrassRenderer once its descriptor set replaces the legacy foliage set.
import Common.SceneData;
import Common.ShadowSampling;
import Grass.GrassPackedData;

[[vk::binding(0, 0)]] Sampler2DShadow shadowMap;
[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(2, 0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(3, 0)]] Sampler2D materialTextures[16];
[[vk::binding(4, 0)]] StructuredBuffer<uint4> shadowPageTable;
[[vk::binding(5, 0)]] StructuredBuffer<GrassPackedInstance> instances;
[[vk::binding(6, 0)]] StructuredBuffer<uint> visibleInstances;
[[vk::binding(7, 0)]] StructuredBuffer<GrassPackedCluster> clusters;

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
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input, uint instanceIndex : SV_InstanceID,
                        uint baseInstance : SV_StartInstanceLocation)
{
    GrassPackedInstance instance = instances[visibleInstances[baseInstance + instanceIndex]];
    GrassPackedCluster cluster = clusters[instance.flags];
    float scale = grassPackedScale(instance);
    float yaw = grassPackedYaw(instance);
    float3 local = grassRotateYaw(input.position * scale, yaw);
    float3 worldPosition = grassPackedWorldPosition(instance, cluster) + local;
    VertexOutput output;
    output.position = mul(mul(float4(worldPosition, 1.0), frame.view), frame.projection);
    output.color = input.color;
    output.worldPosition = worldPosition;
    output.normal = grassRotateYaw(input.normal, yaw);
    output.materialIndex = cluster.instanceRange.z + input.materialIndex;
    output.texCoord = input.texCoord;
    return output;
}

struct FragmentInput
{
    [[vk::location(0)]] float3 color;
    [[vk::location(1)]] float3 worldPosition;
    [[vk::location(2)]] float3 normal;
    [[vk::location(3)]] nointerpolation uint materialIndex;
    [[vk::location(4)]] float2 texCoord;
    bool isFrontFace : SV_IsFrontFace;
};

[shader("fragment")]
float4 fragmentMain(FragmentInput input) : SV_Target
{
    MaterialData material = materials[input.materialIndex];
    float4 albedo = float4(input.color * material.baseColorMetallic.rgb, 1.0);
    if (material.textureIndices.x >= 0) albedo *= materialTextures[material.textureIndices.x].Sample(input.texCoord);
    if (albedo.a < max(material.roughnessAmbientOcclusion.z, 0.01)) discard;
    float3 normal = normalize(input.isFrontFace ? input.normal : -input.normal);
    float3 lightDirection = normalize(-frame.lightDirectionIntensity.xyz);
    float diffuse = saturate(dot(normal, lightDirection) * 0.5 + 0.5);
    float shadow = frame.shadowEnabled != 0 ? sampleVirtualShadow(
        shadowMap, input.worldPosition, frame.shadowClipMatrices, shadowPageTable) : 0.0;
    return float4(albedo.rgb * (0.20 + diffuse * frame.lightDirectionIntensity.w * (1.0 - shadow) * frame.lightColor.rgb), 1.0);
}
