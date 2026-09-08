import Grass.GrassPackedData;
struct Uniforms { uint visibleCapacity; uint streamIndex; uint clusterCount; uint _pad; };
[[vk::binding(0,0)]] StructuredBuffer<GrassPackedInstance> instances;
[[vk::binding(1,0)]] StructuredBuffer<uint> visible;
[[vk::binding(2,0)]] StructuredBuffer<uint> classifyCounts;
[[vk::binding(3,0)]] RWStructuredBuffer<uint> binCounts;
[[vk::binding(4,0)]] ConstantBuffer<Uniforms> uniforms;
[numthreads(64,1,1)] [shader("compute")]
void main(uint3 id : SV_DispatchThreadID) {
    uint i=id.x; if(i>=min(classifyCounts[uniforms.streamIndex], uniforms.visibleCapacity)) return;
    uint cluster=instances[visible[i]].flags; if(cluster>=uniforms.clusterCount) return;
    uint ignored; InterlockedAdd(binCounts[cluster],1u,ignored);
}
