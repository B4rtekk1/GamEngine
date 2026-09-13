// Opaque one-sided VSM casters: vertex-to-depth only.  Alpha-tested and
// foliage casters remain in shadow_map.slang, where discard is required.
import Common.SceneData;

struct PushConstants { float4x4 lightSpace; };
[[vk::binding(5, 0)]] StructuredBuffer<RendererInstanceData> visibleInstances;
[[vk::binding(6, 0)]] StructuredBuffer<uint> visibleInstanceIndices;
[[vk::push_constant]] ConstantBuffer<PushConstants> pushConstants;

struct ShadowVertexInput { [[vk::location(0)]] float3 position : POSITION; };
struct ShadowVertexOutput { float4 position : SV_Position; };

[shader("vertex")]
ShadowVertexOutput vertexMain(ShadowVertexInput input, uint instanceIndex : SV_InstanceID,
                              uint baseInstance : SV_StartInstanceLocation)
{
    RendererInstanceData instance = visibleInstances[visibleInstanceIndices[baseInstance + instanceIndex]];
    float4 q = instance.rotation;
    float3x3 rotationMatrix = float3x3(
        1.0 - 2.0 * (q.y*q.y + q.z*q.z), 2.0 * (q.x*q.y + q.z*q.w), 2.0 * (q.x*q.z - q.y*q.w),
        2.0 * (q.x*q.y - q.z*q.w), 1.0 - 2.0 * (q.x*q.x + q.z*q.z), 2.0 * (q.y*q.z + q.x*q.w),
        2.0 * (q.x*q.z + q.y*q.w), 2.0 * (q.y*q.z - q.x*q.w), 1.0 - 2.0 * (q.x*q.x + q.y*q.y));
    ShadowVertexOutput output;
    const float3 world = mul(input.position * instance.scaleBase.xyz, rotationMatrix) + instance.positionMaterial.xyz;
    output.position = mul(float4(world, 1.0), pushConstants.lightSpace);
    return output;
}
