[[vk::binding(0,1)]] Sampler2D sourceMip;
[[vk::binding(1,1)]] RWTexture2D<float> destinationMip;
[numthreads(8,8,1)] [shader("compute")]
void main(uint3 id:SV_DispatchThreadID){
 uint dw,dh;destinationMip.GetDimensions(dw,dh);if(id.x>=dw||id.y>=dh)return;
 uint sw,sh,mips;sourceMip.GetDimensions(0,sw,sh,mips);uint2 b=id.xy*2u;
 // Mips round down, so on odd-sized sources the last destination texel also
 // owns the trailing source row/column. Folding it in keeps the MIN hierarchy
 // conservative for ray traversal.
 uint2 last=uint2(id.x+1u==dw&&sw>2u*dw?2u:1u,id.y+1u==dh&&sh>2u*dh?2u:1u);
 float result=1e30;
 for(uint y=0u;y<=last.y;++y)for(uint x=0u;x<=last.x;++x)
  result=min(result,sourceMip.Load(int3(min(b+uint2(x,y),uint2(sw-1,sh-1)),0)).r);
 destinationMip[id.xy]=result;
}
