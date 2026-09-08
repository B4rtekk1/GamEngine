import Grass.GrassPackedData;

// One invocation per already-visible grass instance. The command builder
// consumes these three independent streams; no generic GPUScene object is
// touched here.
struct GrassClassifyUniforms { float mainDistance; float shadowDistance; float velocityDistance; float padding; float4 cameraPosition; };
[[vk::binding(0, 0)]] StructuredBuffer<GrassPackedInstance> instances;
[[vk::binding(1, 0)]] StructuredBuffer<GrassPackedCluster> clusters;
[[vk::binding(2, 0)]] StructuredBuffer<uint> visibleInput;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> mainVisible;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> shadowVisible;
[[vk::binding(5, 0)]] RWStructuredBuffer<uint> velocityVisible;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> counts; // main, shadow, velocity
[[vk::binding(7, 0)]] StructuredBuffer<uint> visibleCount;
[[vk::binding(8, 0)]] ConstantBuffer<GrassClassifyUniforms> uniforms;

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= visibleCount[0]) return;
    uint instanceId = visibleInput[id.x];
    GrassPackedInstance instance = instances[instanceId];
    float distanceToCamera = length(grassPackedWorldPosition(instance, clusters[instance.flags]) - uniforms.cameraPosition.xyz);
    uint destination;
    if (distanceToCamera <= uniforms.mainDistance) { InterlockedAdd(counts[0], 1u, destination); mainVisible[destination] = instanceId; }
    if (distanceToCamera <= uniforms.shadowDistance) { InterlockedAdd(counts[1], 1u, destination); shadowVisible[destination] = instanceId; }
    if (distanceToCamera <= uniforms.velocityDistance) { InterlockedAdd(counts[2], 1u, destination); velocityVisible[destination] = instanceId; }
}
