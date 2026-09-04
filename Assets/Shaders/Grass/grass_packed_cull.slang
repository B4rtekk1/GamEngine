import Grass.GrassPackedData;

// One invocation per compact, packed blade.  This replaces the legacy chunk
// format for the new renderer path; it never reads GPUScene or
// RendererInstanceData.
[[vk::binding(0, 0)]] StructuredBuffer<GrassPackedInstance> instances;
[[vk::binding(1, 0)]] StructuredBuffer<GrassPackedCluster> clusters;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> visibleInstances;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> visibleCount;
struct Uniforms { float4x4 viewProjection; float4 cameraPosition; uint instanceCount; uint _pad0; uint _pad1; uint _pad2; };
[[vk::binding(4, 0)]] ConstantBuffer<Uniforms> culling;

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= culling.instanceCount) return;
    GrassPackedInstance instance = instances[id.x];
    float3 position = grassPackedWorldPosition(instance, clusters[instance.flags]);
    float4 clip = mul(float4(position, 1.0), culling.viewProjection);
    if (clip.w <= 0.0 || any(clip.xyz < float3(-clip.w, -clip.w, 0.0)) ||
        any(clip.xyz > float3(clip.w, clip.w, clip.w))) return;
    uint destination;
    InterlockedAdd(visibleCount[0], 1u, destination);
    visibleInstances[destination] = id.x;
}
