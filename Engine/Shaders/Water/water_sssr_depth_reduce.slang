[[vk::binding(0,1)]] Sampler2D sourceMip;
[[vk::binding(1,1)]] RWTexture2D<float> destinationMip;
[numthreads(8,8,1)] [shader("compute")]
void main(uint3 id:SV_DispatchThreadID){
 uint dw,dh;destinationMip.GetDimensions(dw,dh);if(id.x>=dw||id.y>=dh)return;
 uint sw,sh,mips;sourceMip.GetDimensions(0,sw,sh,mips);uint2 b=id.xy*2u;
 float a=sourceMip.Load(int3(min(b,uint2(sw-1,sh-1)),0)).r;
 float c=sourceMip.Load(int3(min(b+uint2(1,0),uint2(sw-1,sh-1)),0)).r;
 float d=sourceMip.Load(int3(min(b+uint2(0,1),uint2(sw-1,sh-1)),0)).r;
 float e=sourceMip.Load(int3(min(b+uint2(1,1),uint2(sw-1,sh-1)),0)).r;
 destinationMip[id.xy]=min(min(a,c),min(d,e));
}
