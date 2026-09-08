struct Uniforms { uint visibleCapacity; uint streamIndex; uint clusterCount; uint _pad; };
[[vk::binding(0,0)]] StructuredBuffer<uint> counts;
[[vk::binding(1,0)]] RWStructuredBuffer<uint> offsets;
[[vk::binding(2,0)]] ConstantBuffer<Uniforms> uniforms;
[numthreads(1,1,1)] [shader("compute")]
void main(uint3 id : SV_DispatchThreadID) { uint offset=0; for(uint i=0;i<uniforms.clusterCount;++i){ offsets[i]=offset; offset+=counts[i]; } }
