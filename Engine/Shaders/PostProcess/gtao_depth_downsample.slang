[[vk::binding(0, 0)]] Sampler2D sourceMip;
[[vk::binding(1, 0)]] RWTexture2D<float> destinationMip;
struct Settings { float effectRadius, effectFalloffRange; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;
static const float BackgroundDepth = 65500.0;

// XeGTAO's depth MIP filter retains the farthest depth in a discontinuity
// while still averaging values on a continuous slope.  The far-depth bias
// prevents a 2x2 footprint spanning foreground and background geometry from
// inventing a surface between them.
float depthMipFilter(float d0, float d1, float d2, float d3) {
    float maxDepth = max(max(d0, d1), max(d2, d3));
    const float depthRangeScaleFactor = 0.75;
    float effectRadius = depthRangeScaleFactor * settings.effectRadius;
    float falloffRange = settings.effectFalloffRange * effectRadius;
    float falloffFrom = effectRadius * (1.0 - settings.effectFalloffRange);
    float falloffMul = -1.0 / falloffRange;
    float falloffAdd = falloffFrom / falloffRange + 1.0;
    float w0 = saturate((maxDepth - d0) * falloffMul + falloffAdd);
    float w1 = saturate((maxDepth - d1) * falloffMul + falloffAdd);
    float w2 = saturate((maxDepth - d2) * falloffMul + falloffAdd);
    float w3 = saturate((maxDepth - d3) * falloffMul + falloffAdd);
    float weightSum = w0 + w1 + w2 + w3;
    return (w0 * d0 + w1 * d1 + w2 * d2 + w3 * d3) / weightSum;
}

[numthreads(8, 8, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID) {
    uint w,h; destinationMip.GetDimensions(w,h); if(any(id.xy>=uint2(w,h))) return;
    uint sw,sh; sourceMip.GetDimensions(sw,sh); int2 size=int2(sw,sh), base=int2(id.xy)*2;
    float d0=sourceMip.Load(int3(min(base+int2(0,0),size-1),0)).r;
    float d1=sourceMip.Load(int3(min(base+int2(1,0),size-1),0)).r;
    float d2=sourceMip.Load(int3(min(base+int2(0,1),size-1),0)).r;
    float d3=sourceMip.Load(int3(min(base+int2(1,1),size-1),0)).r;
    // Do not include the sky sentinel in the geometric-depth filter.
    bool g0=d0<BackgroundDepth, g1=d1<BackgroundDepth;
    bool g2=d2<BackgroundDepth, g3=d3<BackgroundDepth;
    uint count=uint(g0)+uint(g1)+uint(g2)+uint(g3);
    if(count==0){destinationMip[id.xy]=BackgroundDepth;return;}
    // With fewer than four geometric samples, duplicating a valid depth keeps
    // the XeGTAO filter normalized without treating the sky as a surface.
    float fallback=g0?d0:(g1?d1:(g2?d2:d3));
    destinationMip[id.xy]=depthMipFilter(g0?d0:fallback,g1?d1:fallback,
                                         g2?d2:fallback,g3?d3:fallback);
}
