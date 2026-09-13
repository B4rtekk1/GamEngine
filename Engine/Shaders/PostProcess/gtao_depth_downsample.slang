[[vk::binding(0, 0)]] Sampler2D sourceMip;
[[vk::binding(1, 0)]] RWTexture2D<float> destinationMip;
static const float BackgroundDepth = 65500.0;
[numthreads(8, 8, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID) {
    uint w,h; destinationMip.GetDimensions(w,h); if(any(id.xy>=uint2(w,h))) return;
    uint sw,sh; sourceMip.GetDimensions(sw,sh); int2 size=int2(sw,sh), base=int2(id.xy)*2;
    float d0=sourceMip.Load(int3(min(base+int2(0,0),size-1),0)).r;
    float d1=sourceMip.Load(int3(min(base+int2(1,0),size-1),0)).r;
    float d2=sourceMip.Load(int3(min(base+int2(0,1),size-1),0)).r;
    float d3=sourceMip.Load(int3(min(base+int2(1,1),size-1),0)).r;
    // Never blend the sky sentinel into geometric depth: on silhouettes a
    // box average would create a fictitious surface tens of kilometres away.
    float sum=0.0; uint count=0;
    if(d0<BackgroundDepth){sum+=d0;++count;} if(d1<BackgroundDepth){sum+=d1;++count;}
    if(d2<BackgroundDepth){sum+=d2;++count;} if(d3<BackgroundDepth){sum+=d3;++count;}
    destinationMip[id.xy]=count>0?sum/float(count):BackgroundDepth;
}
