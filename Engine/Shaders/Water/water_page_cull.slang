import Common.SceneData;
import Culling.CullingTypes;

struct GPUVirtualWaterPage {
    float originX, originZ, baseHeight, cellSize;
    float minX, minZ, maxX, maxZ;
    float verticalBound, horizontalBound, unresolvedSlopeVariance, reservedFloat;
    uint level, stitchMask, waveCandidateMask, flags;
    uint instanceIndex, drawBin, bodyIndex, spectralEdgeMask;
    float flowX, flowZ, flowSpeed, reservedFlow;
    uint neighborLeft0, neighborLeft1, neighborRight0, neighborRight1;
    uint neighborBottom0, neighborBottom1, neighborTop0, neighborTop1;
};
struct GPUVisibleWaterPage { uint pageIndex, waveCandidateMask, packedQuality, instanceIndex; };
struct GPUWaterPageHistory { uint packedQuality, visibleAge; float previousImportance, reserved; };
struct GPUWaterPageStats { uint visiblePages, frustumRejected, hiZRejected, distanceRejected; };
struct GPUWaterCullConfig {
    uint pageCount, drawBinCount, visiblePagesPerBin, enableHiZ;
    float reflectionVarianceScale, highQualityPixels, mediumQualityPixels, lowQualityPixels;
};

[[vk::binding(0,0)]] StructuredBuffer<GPUVirtualWaterPage> pages;
[[vk::binding(1,0)]] StructuredBuffer<RendererInstanceData> instances;
[[vk::binding(2,0)]] RWStructuredBuffer<GPUVisibleWaterPage> visiblePages;
[[vk::binding(3,0)]] RWStructuredBuffer<uint> drawBinCounts;
[[vk::binding(4,0)]] Sampler2D previousHiZ;
[[vk::binding(5,0)]] ConstantBuffer<CullingUniformData> culling;
[[vk::binding(6,0)]] StructuredBuffer<GPUWaterPageHistory> previousHistory;
[[vk::binding(7,0)]] RWStructuredBuffer<GPUWaterPageHistory> currentHistory;
[[vk::binding(8,0)]] RWStructuredBuffer<GPUWaterPageStats> stats;
[[vk::binding(9,0)]] ConstantBuffer<GPUWaterCullConfig> config;

float3 rotateQuaternion(float3 p, float4 q) {
    return p + 2.0 * cross(q.xyz, cross(q.xyz, p) + q.w * p);
}
float3 localToWorld(float3 p, RendererInstanceData i) {
    return rotateQuaternion(p * i.scaleBase.xyz, i.rotation) + i.positionMaterial.xyz;
}

void buildCorners(GPUVirtualWaterPage page, RendererInstanceData instance,
                  out float4 clips[8], out float3 worldCorners[8]) {
    float3 lo = float3(page.minX - page.horizontalBound,
                       page.baseHeight - page.verticalBound,
                       page.minZ - page.horizontalBound);
    float3 hi = float3(page.maxX + page.horizontalBound,
                       page.baseHeight + page.verticalBound,
                       page.maxZ + page.horizontalBound);
    // The sparse interaction solver stores height in world metres and the
    // prepass applies it along the body's rotated local-up axis without scale.
    // Expand after localToWorld so non-unit/negative Y scale cannot under-bound
    // a wake or local ripple.
    const float3 interactionOffset = rotateQuaternion(float3(0.0,0.75,0.0), instance.rotation);
    [unroll] for (uint n=0; n<8; ++n) {
        float3 p = float3(((n&1u)!=0u)?hi.x:lo.x, ((n&2u)!=0u)?hi.y:lo.y, ((n&4u)!=0u)?hi.z:lo.z);
        worldCorners[n] = localToWorld(p, instance) + (((n&2u)!=0u)?interactionOffset:-interactionOffset);
        clips[n] = mul(float4(worldCorners[n],1.0), culling.viewProjection);
    }
}

bool outsideFrustum(float4 clips[8]) {
    bool L=true,R=true,B=true,T=true,N=true,F=true;
    [unroll] for (uint i=0;i<8;++i) {
        float4 c=clips[i];
        if(c.w<=1e-5 || c.z<=0.0) return false;
        L &= c.x < -c.w; R &= c.x > c.w; B &= c.y < -c.w; T &= c.y > c.w;
        N &= c.z < 0.0; F &= c.z > c.w;
    }
    return L||R||B||T||N||F;
}

float projectedExtentPixels(float4 clips[8], out float2 minUv, out float2 maxUv, out float nearestDepth) {
    float2 mn=1.0, mx=-1.0; nearestDepth=1.0;
    [unroll] for(uint i=0;i<8;++i) {
        if(clips[i].w<=1e-5) { minUv=0; maxUv=1; nearestDepth=0; return max(culling.viewportWidth,culling.viewportHeight); }
        float3 ndc=clips[i].xyz/clips[i].w;
        mn=min(mn,ndc.xy); mx=max(mx,ndc.xy); nearestDepth=min(nearestDepth,ndc.z);
    }
    mn=clamp(mn,-1.0,1.0); mx=clamp(mx,-1.0,1.0);
    minUv=mn*0.5+0.5; maxUv=mx*0.5+0.5;
    minUv.y=1.0-minUv.y; maxUv.y=1.0-maxUv.y;
    float a=min(minUv.y,maxUv.y), b=max(minUv.y,maxUv.y); minUv.y=a; maxUv.y=b;
    float2 px=(maxUv-minUv)*float2(culling.viewportWidth,culling.viewportHeight);
    return max(px.x,px.y);
}

bool occluded(float2 minUv,float2 maxUv,float nearestDepth) {
    if(config.enableHiZ==0 || culling.enableOcclusionCulling==0 || culling.cameraCut!=0 || culling.hiZMipCount==0) return false;
    float2 px=(maxUv-minUv)*float2(culling.viewportWidth,culling.viewportHeight);
    uint mip=uint(clamp(ceil(log2(max(max(px.x,px.y),1.0))),0.0,float(culling.hiZMipCount-1)));
    uint w,h,mips; previousHiZ.GetDimensions(mip,w,h,mips);
    int2 size=int2(w,h);
    int2 lo=clamp(int2(floor(minUv*float2(size))),int2(0),size-1);
    int2 hi=clamp(int2(floor(maxUv*float2(size))),int2(0),size-1);
    float region=0.0;
    for(int y=lo.y;y<=hi.y;++y) for(int x=lo.x;x<=hi.x;++x)
        region=max(region,previousHiZ.Load(int3(x,y,int(mip))).r);
    return nearestDepth > region + culling.depthBias;
}

uint chooseTier(float pixels, float variance, uint previousTier) {
    float hi=config.highQualityPixels, med=config.mediumQualityPixels, low=config.lowQualityPixels;
    uint tier = pixels>=hi?3u : pixels>=med?2u : pixels>=low?1u : 0u;

    // 15% hysteresis around the tier being entered/exited avoids temporal
    // oscillation when projected page size hovers around a threshold.
    if(tier>previousTier){
        float enter=tier==3u?hi:(tier==2u?med:low);
        if(pixels<enter*1.15) tier=previousTier;
    } else if(tier<previousTier){
        float exit=previousTier==3u?hi:(previousTier==2u?med:low);
        if(pixels>exit*0.85) tier=previousTier;
    }

    // Large unresolved slopes naturally prefer a cheaper, blurrier reflection path.
    if(variance*config.reflectionVarianceScale > 0.18 && tier>0) tier--;
    return tier;
}

[numthreads(64,1,1)]
[shader("compute")]
void main(uint3 tid:SV_DispatchThreadID) {
    uint id=tid.x;
    bool visible=false, frustumReject=false, hizReject=false, distanceReject=false;
    GPUVirtualWaterPage page=(GPUVirtualWaterPage)0;
    GPUVisibleWaterPage outPage=(GPUVisibleWaterPage)0;
    uint packed=0; float importance=0.0;
    if(id<config.pageCount) {
        page=pages[id];
        if((page.flags&1u)!=0) {
            RendererInstanceData instance=instances[page.instanceIndex];
            float4 clips[8]; float3 worldCorners[8]; buildCorners(page,instance,clips,worldCorners);
            frustumReject = culling.enableFrustumCulling!=0 && outsideFrustum(clips);
            if(!frustumReject) {
                float2 minUv,maxUv; float nearDepth;
                float pixels=projectedExtentPixels(clips,minUv,maxUv,nearDepth);
                distanceReject = pixels < 0.75;
                if(!distanceReject) hizReject=occluded(minUv,maxUv,nearDepth);
                if(!hizReject && !distanceReject) {
                    visible=true; importance=pixels;
                    uint prev=previousHistory[id].packedQuality;
                    uint reflection=chooseTier(pixels,page.unresolvedSlopeVariance,prev&3u);
                    uint refraction=chooseTier(pixels,0.0,(prev>>2u)&3u);
                    uint foam=(page.level<=1 && pixels>=config.mediumQualityPixels)?2u:(page.level<=2&&pixels>=config.lowQualityPixels?1u:0u);
                    bool reactive = previousHistory[id].visibleAge==0 || (prev&0x3fu)!=(reflection|(refraction<<2)|(foam<<4));
                    packed=reflection|(refraction<<2)|(foam<<4)|((id&0x1fffu)<<8)|(reactive?0x40000000u:0u)|0x80000000u;
                    outPage={id,page.waveCandidateMask,packed,page.instanceIndex};
                }
            }
        }
    }
    if(id<config.pageCount) {
        GPUWaterPageHistory h; h.packedQuality=packed; h.visibleAge=visible?min(previousHistory[id].visibleAge+1u,255u):0u;
        h.previousImportance=importance; h.reserved=0; currentHistory[id]=h;
    }
    // 448 logical pages are tiny enough that one atomic per visible page is cheaper
    // and, crucially, correct even when one wave spans several stitch/body bins.
    if(visible) {
        uint dst=0; InterlockedAdd(drawBinCounts[page.drawBin],1,dst);
        if(dst<config.visiblePagesPerBin) {
            uint slot=page.drawBin*config.visiblePagesPerBin+dst;
            visiblePages[slot]=outPage;
            InterlockedAdd(stats[0].visiblePages,1);
        }
    }
    if(id<config.pageCount && !visible) {
        if(frustumReject) InterlockedAdd(stats[0].frustumRejected,1);
        else if(hizReject) InterlockedAdd(stats[0].hiZRejected,1);
        else if(distanceReject) InterlockedAdd(stats[0].distanceRejected,1);
    }
}
