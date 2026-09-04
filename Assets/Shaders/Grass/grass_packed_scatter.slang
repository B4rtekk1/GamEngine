import Grass.GrassPackedData;
struct Uniforms { uint visibleCapacity; uint streamIndex; uint clusterCount; uint _pad; };
[[vk::binding(0,0)]] StructuredBuffer<GrassPackedInstance> instances;
[[vk::binding(1,0)]] StructuredBuffer<uint> visible;
[[vk::binding(2,0)]] StructuredBuffer<uint> classifyCounts;
[[vk::binding(3,0)]] StructuredBuffer<uint> offsets;
[[vk::binding(4,0)]] RWStructuredBuffer<uint> cursors;
[[vk::binding(5,0)]] RWStructuredBuffer<uint> sorted;
[[vk::binding(6,0)]] ConstantBuffer<Uniforms> uniforms;
[numthreads(64,1,1)] [shader("compute")]
void main(uint3 id : SV_DispatchThreadID) { uint i=id.x; if(i>=min(classifyCounts[uniforms.streamIndex],uniforms.visibleCapacity))return; uint instanceId=visible[i]; uint cluster=instances[instanceId].flags; if(cluster>=uniforms.clusterCount)return; uint local; InterlockedAdd(cursors[cluster],1u,local); sorted[offsets[cluster]+local]=instanceId; }
