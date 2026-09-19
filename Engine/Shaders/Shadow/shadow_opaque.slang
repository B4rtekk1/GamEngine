// Opaque one-sided VSM casters: vertex-to-depth only.  Alpha-tested and
// foliage casters remain in shadow_map.slang, where discard is required.
import Common.SceneData;

struct PushConstants { float4x4 lightSpace; };
struct ShadowInstanceTransform { float4 row0; float4 row1; float4 row2; };
[[vk::binding(5, 0)]] StructuredBuffer<ShadowInstanceTransform> shadowTransforms;
[[vk::push_constant]] ConstantBuffer<PushConstants> pushConstants;

struct ShadowVertexInput { [[vk::location(0)]] float3 position : POSITION; };
struct ShadowVertexOutput { float4 position : SV_Position; };

[shader("vertex")]
ShadowVertexOutput vertexMain(ShadowVertexInput input, uint instanceIndex : SV_InstanceID,
                              uint baseInstance : SV_StartInstanceLocation)
{
    const ShadowInstanceTransform transform = shadowTransforms[baseInstance + instanceIndex];
    ShadowVertexOutput output;
    const float3 world = float3(dot(transform.row0, float4(input.position, 1.0)),
                                dot(transform.row1, float4(input.position, 1.0)),
                                dot(transform.row2, float4(input.position, 1.0)));
    output.position = mul(float4(world, 1.0), pushConstants.lightSpace);
    return output;
}
