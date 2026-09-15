import Water.VirtualWaterShared;
struct IndexedDrawCommand { uint indexCount, instanceCount, firstIndex; int vertexOffset; uint firstInstance; };
[[vk::binding(0,0)]] StructuredBuffer<uint> counts;
[[vk::binding(1,0)]] StructuredBuffer<GPUWaterDrawTemplate> templates;
[[vk::binding(2,0)]] RWStructuredBuffer<IndexedDrawCommand> commands;
[[vk::binding(3,0)]] ConstantBuffer<GPUWaterCullConfig> config;
[numthreads(64,1,1)] [shader("compute")]
void main(uint3 tid:SV_DispatchThreadID){
 uint i=tid.x; if(i>=config.drawBinCount)return; GPUWaterDrawTemplate t=templates[i];
 IndexedDrawCommand c; c.indexCount=t.indexCount; c.instanceCount=min(counts[i],config.visiblePagesPerBin);
 c.firstIndex=t.firstIndex; c.vertexOffset=t.vertexOffset; c.firstInstance=t.firstInstanceBase; commands[i]=c;
}
