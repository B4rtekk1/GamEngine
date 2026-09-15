import Common.SceneData;
import Culling.CullingTypes;
import Water.VirtualWaterShared;

[[vk::binding(1,0)]] ConstantBuffer<RendererFrameData> frame;

[[vk::binding(0,1)]] StructuredBuffer<GPUVirtualWaterPage> pages;
[[vk::binding(1,1)]] StructuredBuffer<RendererInstanceData> instances;
[[vk::binding(2,1)]] RWStructuredBuffer<GPUVisibleWaterPage> visiblePages;
[[vk::binding(3,1)]] RWStructuredBuffer<uint> drawBinCounts;
[[vk::binding(4,1)]] Sampler2D previousHiZ;
[[vk::binding(5,1)]] ConstantBuffer<CullingUniformData> culling;
[[vk::binding(6,1)]] StructuredBuffer<GPUWaterPageHistory> previousHistory;
[[vk::binding(7,1)]] RWStructuredBuffer<GPUWaterPageHistory> currentHistory;
[[vk::binding(8,1)]] RWStructuredBuffer<GPUWaterPageStats> stats;
[[vk::binding(9,1)]] ConstantBuffer<GPUWaterCullConfig> config;
[[vk::binding(10,1)]] RWStructuredBuffer<uint> candidateMask;

void buildCorners(GPUVirtualWaterPage page, RendererInstanceData instance,
                  out float4 currentClips[8], out float4 previousClips[8]) {
    const float2 snapped = waterSnappedOffset(page, instance, frame.cameraPosition.xyz);
    const float baseLow=(page.flags&WATER_PAGE_SPLINE)!=0u?min(page.baseHeight,page.reservedFloat):page.baseHeight;
    const float baseHigh=(page.flags&WATER_PAGE_SPLINE)!=0u?max(page.baseHeight,page.reservedFloat):page.baseHeight;
    float3 lo = float3(page.minX + snapped.x - page.horizontalBound,
                       baseLow - page.verticalBound,
                       page.minZ + snapped.y - page.horizontalBound);
    float3 hi = float3(page.maxX + snapped.x + page.horizontalBound,
                       baseHigh + page.verticalBound,
                       page.maxZ + snapped.y + page.horizontalBound);
    const float3 interactionOffset = waterRotateQuaternion(float3(0.0, 0.75, 0.0), instance.rotation);
    [unroll] for (uint n=0; n<8; ++n) {
        float3 p = float3(((n&1u)!=0u)?hi.x:lo.x, ((n&2u)!=0u)?hi.y:lo.y, ((n&4u)!=0u)?hi.z:lo.z);
        float3 world = waterLocalToWorld(p, instance) + (((n&2u)!=0u)?interactionOffset:-interactionOffset);
        currentClips[n] = mul(mul(float4(world,1.0), frame.view), frame.projection);
        previousClips[n] = mul(mul(float4(world,1.0), frame.previousView), frame.previousProjection);
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
    float2 px=max((maxUv-minUv)*float2(culling.viewportWidth,culling.viewportHeight),0.0);
    return max(px.x,px.y);
}

bool occludedPrevious(float4 previousClips[8]) {
    if(config.enableHiZ==0 || culling.enableOcclusionCulling==0 || culling.cameraCut!=0 || culling.hiZMipCount==0) return false;
    float2 minUv,maxUv; float nearestDepth;
    projectedExtentPixels(previousClips,minUv,maxUv,nearestDepth);
    if(any(maxUv<=0.0)||any(minUv>=1.0)) return false;
    minUv=saturate(minUv); maxUv=saturate(maxUv);
    float2 px=max((maxUv-minUv)*float2(culling.viewportWidth,culling.viewportHeight),1.0);
    uint mip=uint(clamp(ceil(log2(max(px.x,px.y))),0.0,float(culling.hiZMipCount-1)));
    uint w,h,mips; previousHiZ.GetDimensions(mip,w,h,mips);
    int2 size=int2(w,h);
    int2 lo=clamp(int2(floor(minUv*float2(size))),int2(0),size-1);
    int2 hi=clamp(int2(floor(maxUv*float2(size))),int2(0),size-1);
    // MAX-depth hierarchy: if the nearest water bound is behind the farthest
    // opaque depth in the covered region the complete page is occluded.
    float region=0.0;
    for(int y=lo.y;y<=hi.y;++y) for(int x=lo.x;x<=hi.x;++x)
        region=max(region,previousHiZ.Load(int3(x,y,int(mip))).r);
    return nearestDepth > region + culling.depthBias;
}

uint chooseTier(float pixels, float variance, uint previousTier, uint framesInTier) {
    const float scale=clamp(config.qualityScale,0.35,1.75);
    float hi=config.highQualityPixels/scale, med=config.mediumQualityPixels/scale, low=config.lowQualityPixels/scale;
    uint tier = pixels>=hi?3u : pixels>=med?2u : pixels>=low?1u : 0u;
    if(tier>previousTier){
        float enter=tier==3u?hi:(tier==2u?med:low);
        if(pixels<enter*1.12 || framesInTier<4u) tier=previousTier;
    } else if(tier<previousTier){
        float exit=previousTier==3u?hi:(previousTier==2u?med:low);
        if(pixels>exit*0.82 || framesInTier<4u) tier=previousTier;
    }
    if(variance*config.reflectionVarianceScale > 0.18 && tier>1u) tier--;
    return tier;
}

[numthreads(64,1,1)]
[shader("compute")]
void main(uint3 tid:SV_DispatchThreadID) {
    uint id=tid.x;
    if(id>=config.pageCount) return;

    GPUVirtualWaterPage page=pages[id];
    GPUWaterPageHistory prev=previousHistory[id];
    GPUWaterPageHistory hist=(GPUWaterPageHistory)0;
    bool visible=false, frustumReject=false, hizReject=false, distanceReject=false;
    uint packed=0; float importance=0.0;
    const bool geometryActive=(page.flags&WATER_PAGE_ACTIVE)!=0u && page.coverageClass!=0u;
    RendererInstanceData instance=instances[page.instanceIndex];
    int2 worldCoord=waterPageWorldCoord(page,instance,frame.cameraPosition.xyz);
    bool sameIdentity=prev.worldPageX==worldCoord.x && prev.worldPageZ==worldCoord.y;

    GPUVisibleWaterPage outPage=(GPUVisibleWaterPage)0;
    if(geometryActive) {
        float4 currentClips[8]; float4 previousClips[8]; buildCorners(page,instance,currentClips,previousClips);
        frustumReject = culling.enableFrustumCulling!=0 && outsideFrustum(currentClips);
        if(!frustumReject) {
            float2 minUv,maxUv; float nearDepth;
            float pixels=projectedExtentPixels(currentClips,minUv,maxUv,nearDepth);
            distanceReject = pixels < 0.75;
            if(!distanceReject && sameIdentity && prev.visibleAge>config.occlusionGraceFrames) {
                const bool candidateOccluded=occludedPrevious(previousClips);
                uint streak=candidateOccluded?prev.occludedFrames+1u:0u;
                hizReject=candidateOccluded && streak>=config.occludedFramesRequired;
                hist.occludedFrames=streak;
            }
            if(!hizReject && !distanceReject) {
                visible=true;
                const float projectedArea=max((maxUv.x-minUv.x)*(maxUv.y-minUv.y)*
                    culling.viewportWidth*culling.viewportHeight,1.0);
                // Conservative page->tile candidate rasterization. False positives are
                // allowed; every covered tile is marked so sparse classification has no
                // false negatives. Dense mode simply ignores this mask.
                const uint tilesX=(uint(culling.viewportWidth)+7u)/8u;
                const uint tilesY=(uint(culling.viewportHeight)+7u)/8u;
                int2 tileLo=int2(floor(saturate(minUv)*float2(culling.viewportWidth,culling.viewportHeight)/8.0));
                int2 tileHi=int2(floor(saturate(maxUv)*float2(culling.viewportWidth,culling.viewportHeight)/8.0));
                tileLo=clamp(tileLo,int2(0),int2(max(int(tilesX)-1,0),max(int(tilesY)-1,0)));
                tileHi=clamp(tileHi,int2(0),int2(max(int(tilesX)-1,0),max(int(tilesY)-1,0)));
                [loop] for(int ty=tileLo.y;ty<=tileHi.y;++ty) [loop] for(int tx=tileLo.x;tx<=tileHi.x;++tx){
                    uint ignoredCandidate=0u;InterlockedOr(candidateMask[uint(ty)*tilesX+uint(tx)],1u,ignoredCandidate);
                }
                const float edgeDistance=min(minUv.x,minUv.y);
                const float edgeImportance=1.0+0.35*saturate(1.0-edgeDistance*8.0);
                const float interactionImportance=(page.flags&(WATER_PAGE_INTERACTION|WATER_PAGE_FOAM_RESIDENT))!=0u?1.35:1.0;
                importance=sqrt(projectedArea)*edgeImportance*interactionImportance;

                uint previousReflection=sameIdentity?(prev.packedQuality&3u):0u;
                uint previousRefraction=sameIdentity?((prev.packedQuality>>2u)&3u):0u;
                uint frames=sameIdentity?prev.framesInTier:0u;
                uint reflection=chooseTier(pixels,page.unresolvedSlopeVariance,previousReflection,frames);
                uint refraction=chooseTier(pixels,0.0,previousRefraction,frames);
                const bool simpleShading=(page.executionFlags&WATER_EXEC_SIMPLE_SHADING)!=0u;
                if(simpleShading){reflection=min(reflection,1u);refraction=min(refraction,1u);}

                if(page.unresolvedSlopeVariance>0.12 && reflection>1u) reflection--;
                uint foam=(page.level<=1 && pixels>=config.mediumQualityPixels)?2u:(page.level<=2&&pixels>=config.lowQualityPixels?1u:0u);
                if((page.flags&WATER_PAGE_SHORELINE)!=0u) foam=max(foam,2u);

                if(reflection==3u) {
                    uint highIndex=0; InterlockedAdd(stats[0].highQualityPages,1u,highIndex);
                    if(highIndex>=config.maxHighGeometryPages){reflection=2u;InterlockedAdd(stats[0].budgetDrops,1u);}
                }
                const bool reactive=!sameIdentity || prev.visibleAge==0u ||
                    (prev.packedQuality&0x3fu)!=(reflection|(refraction<<2u)|(foam<<4u));
                packed=reflection|(refraction<<2u)|(foam<<4u)|((id&0x1fffu)<<8u)|
                    (simpleShading?WATER_SIMPLE_SHADING_FLAG:0u)|
                    (reactive?WATER_REACTIVE_FLAG:0u)|WATER_VALID_FLAG;
                outPage={id,page.waveCandidateMask,packed,page.instanceIndex};
                hist.framesInTier=(sameIdentity && (prev.packedQuality&0x3fu)==(packed&0x3fu))?
                    min(prev.framesInTier+1u,255u):0u;
            }
        }
    }

    hist.packedQuality=packed;
    hist.visibleAge=visible?(sameIdentity?min(prev.visibleAge+1u,255u):1u):0u;
    hist.previousImportance=importance;
    hist.worldPageX=worldCoord.x; hist.worldPageZ=worldCoord.y;
    if(!sameIdentity) hist.occludedFrames=0u;
    currentHistory[id]=hist;

    if(visible) {
        uint dst=0; InterlockedAdd(drawBinCounts[page.drawBin],1,dst);
        if(dst<config.visiblePagesPerBin) {
            uint slot=page.drawBin*config.visiblePagesPerBin+dst;
            visiblePages[slot]=outPage;
            InterlockedAdd(stats[0].visiblePages,1u);
            if(!sameIdentity || prev.visibleAge==0u) InterlockedAdd(stats[0].newlyVisible,1u);
        }
    } else {
        if(frustumReject) InterlockedAdd(stats[0].frustumRejected,1u);
        else if(hizReject) InterlockedAdd(stats[0].hiZRejected,1u);
        else if(distanceReject) InterlockedAdd(stats[0].distanceRejected,1u);
    }
}
