[[vk::binding(0, 0)]] Sampler2D sourceMip;
[[vk::binding(1, 0)]] RWTexture2D<float> destinationMip;
[numthreads(8, 8, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID) {
    uint w,h; destinationMip.GetDimensions(w,h); if(any(id.xy>=uint2(w,h))) return;
    uint sw,sh; sourceMip.GetDimensions(sw,sh); int2 size=int2(sw,sh), base=int2(id.xy)*2;
    float d0=sourceMip.Load(int3(min(base+int2(0,0),size-1),0)).r;
    float d1=sourceMip.Load(int3(min(base+int2(1,0),size-1),0)).r;
    float d2=sourceMip.Load(int3(min(base+int2(0,1),size-1),0)).r;
    float d3=sourceMip.Load(int3(min(base+int2(1,1),size-1),0)).r;
    destinationMip[id.xy]=(d0+d1+d2+d3)*0.25;
}
