[[vk::binding(0,0)]] Sampler2D sourceDepth;
[[vk::binding(1,0)]] RWTexture2D<float> destinationDepth;

// Builds half-resolution mip 0 with MIN depth semantics. The ordinary engine
// Hi-Z is MAX depth for conservative object occlusion and cannot be reused for
// SSSR/refraction ray intersections.
[numthreads(8,8,1)]
[shader("compute")]
void main(uint3 id:SV_DispatchThreadID){
    uint dw,dh;destinationDepth.GetDimensions(dw,dh);
    if(id.x>=dw||id.y>=dh)return;
    uint sw,sh;sourceDepth.GetDimensions(sw,sh);
    uint2 base=id.xy*2u;
    uint2 p0=min(base,uint2(sw-1u,sh-1u));
    uint2 p1=min(base+uint2(1,0),uint2(sw-1u,sh-1u));
    uint2 p2=min(base+uint2(0,1),uint2(sw-1u,sh-1u));
    uint2 p3=min(base+uint2(1,1),uint2(sw-1u,sh-1u));
    float d0=sourceDepth.Load(int3(p0,0)).r;
    float d1=sourceDepth.Load(int3(p1,0)).r;
    float d2=sourceDepth.Load(int3(p2,0)).r;
    float d3=sourceDepth.Load(int3(p3,0)).r;
    destinationDepth[id.xy]=min(min(d0,d1),min(d2,d3));
}
