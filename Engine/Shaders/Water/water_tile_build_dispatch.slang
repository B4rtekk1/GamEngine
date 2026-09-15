struct DispatchCommand { uint x,y,z; };
[[vk::binding(0,0)]] StructuredBuffer<uint> counts;
[[vk::binding(1,0)]] RWStructuredBuffer<DispatchCommand> commands;
[numthreads(4,1,1)] [shader("compute")]
void main(uint3 tid:SV_DispatchThreadID){ if(tid.x<4){ DispatchCommand c; c.x=counts[tid.x];c.y=1;c.z=1;commands[tid.x]=c; } }
