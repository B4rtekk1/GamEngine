import Grass.GrassPackedData;
struct Draw { uint indexCount; uint instanceCount; uint firstIndex; int vertexOffset; uint firstInstance; };
struct Uniforms { uint visibleCapacity; uint streamIndex; uint clusterCount; uint _pad; };
[[vk::binding(0,0)]] StructuredBuffer<GrassPackedCluster> clusters;
[[vk::binding(1,0)]] StructuredBuffer<uint> counts;
[[vk::binding(2,0)]] StructuredBuffer<uint> offsets;
[[vk::binding(3,0)]] RWStructuredBuffer<Draw> commands;
[[vk::binding(4,0)]] RWStructuredBuffer<uint> drawCount;
[[vk::binding(5,0)]] ConstantBuffer<Uniforms> uniforms;
[numthreads(64,1,1)] [shader("compute")]
void main(uint3 id : SV_DispatchThreadID) { uint cluster=id.x; if(cluster>=uniforms.clusterCount)return; uint count=counts[cluster]; if(count==0)return; GrassPackedCluster data=clusters[cluster]; Draw cmd; cmd.indexCount=data.draw.y; cmd.instanceCount=count; cmd.firstIndex=data.draw.x; cmd.vertexOffset=int(data.draw.z); cmd.firstInstance=offsets[cluster]; commands[cluster]=cmd; InterlockedMax(drawCount[0],cluster+1); }
