[[vk::binding(0,0)]] Sampler2D sourceMip;
[[vk::binding(1,0)]] RWTexture2D<float> destinationMip;

[numthreads(8,8,1)]
[shader("compute")]
void main(uint3 tid : SV_DispatchThreadID) {
    uint dw, dh; destinationMip.GetDimensions(dw, dh);
    if(any(tid.xy >= uint2(dw,dh))) return;
    uint sw, sh; sourceMip.GetDimensions(sw,sh);
    int2 base = int2(tid.xy) * 2;
    int2 limit = int2(sw,sh)-1;
    float d0=sourceMip.Load(int3(min(base+int2(0,0),limit),0)).r;
    float d1=sourceMip.Load(int3(min(base+int2(1,0),limit),0)).r;
    float d2=sourceMip.Load(int3(min(base+int2(0,1),limit),0)).r;
    float d3=sourceMip.Load(int3(min(base+int2(1,1),limit),0)).r;
    // Vulkan RH_ZO: smaller device depth is the nearest receiver.
    destinationMip[tid.xy]=min(min(d0,d1),min(d2,d3));
}
