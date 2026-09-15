import Water.VirtualWaterShared;

[[vk::binding(0,0)]] Sampler2D waterMeta;
[[vk::binding(1,0)]] Sampler2D waterSurface;
[[vk::binding(2,0)]] RWStructuredBuffer<uint> tileLists;
[[vk::binding(3,0)]] RWStructuredBuffer<uint> tierCounts;
[[vk::binding(4,0)]] ConstantBuffer<GPUWaterTileSettings> settings;
[[vk::binding(5,0)]] RWStructuredBuffer<uint> importanceHistogram;
[[vk::binding(6,0)]] RWStructuredBuffer<GPUWaterFrameBudget> budgets;
[[vk::binding(7,0)]] StructuredBuffer<uint> candidateMask;

groupshared float4 groupSurface[64];
groupshared uint groupQuality[64];
groupshared uint groupMaterial[64];
groupshared uint groupHasWater;
groupshared uint groupReflection;
groupshared uint groupRefraction;
groupshared uint groupErrorBits;
groupshared uint groupImportanceBits;
groupshared uint groupWaterCount;
groupshared uint groupSimpleCount;

uint laneIndex(uint2 local){return local.y*8u+local.x;}
uint packTile(uint tile,uint rate,uint reflectionCap,uint refractionCap){
    return (tile&0x00ffffffu)|((rate&3u)<<24u)|((reflectionCap&3u)<<26u)|((refractionCap&3u)<<28u);
}

// Lock-free bounded reservation. Failed contention is treated conservatively as
// a budget miss, so the GPU work cap can never be exceeded by atomics racing.
bool reserveBudget(uint counterKind,uint amount,uint limit){
    if(amount==0u)return true;
    [loop]for(uint attempt=0u;attempt<16u;++attempt){
        uint current=counterKind==0u?budgets[0].reflectionUsed:
                     (counterKind==1u?budgets[0].refractionUsed:budgets[0].fullRateUsed);
        if(current>limit||amount>limit-current)return false;
        uint original=0u;
        if(counterKind==0u)InterlockedCompareExchange(budgets[0].reflectionUsed,current,current+amount,original);
        else if(counterKind==1u)InterlockedCompareExchange(budgets[0].refractionUsed,current,current+amount,original);
        else InterlockedCompareExchange(budgets[0].fullRateUsed,current,current+amount,original);
        if(original==current)return true;
    }
    return false;
}

[numthreads(8,8,1)]
[shader("compute")]
void main(uint3 gid:SV_GroupID,uint3 lid3:SV_GroupThreadID,uint3 tid:SV_DispatchThreadID){
    uint2 lid=lid3.xy;uint lane=laneIndex(lid);uint tileIndex=gid.y*settings.tilesX+gid.x;
    if(settings.denseMode==0u&&candidateMask[tileIndex]==0u)return;
    if(lane==0u){groupHasWater=0u;groupReflection=0u;groupRefraction=0u;groupErrorBits=0u;groupImportanceBits=0u;groupWaterCount=0u;groupSimpleCount=0u;}
    bool inBounds=tid.x<settings.width&&tid.y<settings.height;
    float4 surface=0;uint quality=0;uint material=0;bool valid=false;
    if(inBounds){float2 meta=waterMeta.Load(int3(tid.xy,0)).xy;quality=asuint(meta.y);material=asuint(meta.x);valid=(quality&WATER_VALID_FLAG)!=0u;if(valid)surface=waterSurface.Load(int3(tid.xy,0));}
    groupSurface[lane]=surface;groupQuality[lane]=quality;groupMaterial[lane]=material;
    if(valid){uint ignored=0;InterlockedOr(groupHasWater,1u,ignored);InterlockedMax(groupReflection,quality&3u);InterlockedMax(groupRefraction,(quality>>2u)&3u);InterlockedAdd(groupWaterCount,1u,ignored);if((quality&WATER_SIMPLE_SHADING_FLAG)!=0u)InterlockedAdd(groupSimpleCount,1u,ignored);}
    GroupMemoryBarrierWithGroupSync();
    if(valid){
        float normalGradient=0.0;float depthGradient=0.0;float edgeMask=0.0;float materialDiscontinuity=0.0;
        if(lid.x<7u){uint n=lane+1u;bool nv=(groupQuality[n]&WATER_VALID_FLAG)!=0u;if(nv){normalGradient=max(normalGradient,length(surface.xy-groupSurface[n].xy));depthGradient=max(depthGradient,abs(surface.z-groupSurface[n].z));materialDiscontinuity=max(materialDiscontinuity,groupMaterial[n]!=material?1.0:0.0);}else edgeMask=1.0;}
        else edgeMask=1.0;
        if(lid.y<7u){uint n=lane+8u;bool nv=(groupQuality[n]&WATER_VALID_FLAG)!=0u;if(nv){normalGradient=max(normalGradient,length(surface.xy-groupSurface[n].xy));depthGradient=max(depthGradient,abs(surface.z-groupSurface[n].z));materialDiscontinuity=max(materialDiscontinuity,groupMaterial[n]!=material?1.0:0.0);}else edgeMask=1.0;}
        else edgeMask=1.0;
        float specularImportance=saturate(1.0-surface.w)*float(max(quality&3u,(quality>>2u)&3u))/3.0;
        float shoreline=edgeMask;
        float error=saturate(normalGradient*2.2+depthGradient*14.0+specularImportance*0.35+edgeMask*0.85+shoreline*0.25+materialDiscontinuity);
        float importance=saturate(error*0.65+specularImportance*0.35);
        uint ignored=0;InterlockedMax(groupErrorBits,asuint(error),ignored);InterlockedMax(groupImportanceBits,asuint(importance),ignored);
    }
    GroupMemoryBarrierWithGroupSync();
    if(lane!=0u||groupHasWater==0u)return;

    float error=asfloat(groupErrorBits);float importance=asfloat(groupImportanceBits);
    uint quantized=min(uint(importance*255.0+0.5),255u);uint ignored=0;InterlockedAdd(importanceHistogram[quantized],1u,ignored);
    uint rate=error>0.42?0u:(error>0.16?1u:2u); // 1x1, 2x2, 4x4 software shading rate
    uint reflectionCap=groupReflection;uint refractionCap=groupRefraction;
    const bool simpleOnly=groupSimpleCount==groupWaterCount;
    if(simpleOnly){reflectionCap=min(reflectionCap,1u);refractionCap=min(refractionCap,1u);rate=2u;}
    // Histogram-derived thresholds redistribute the fixed frame budget toward
    // the most important tiles. Tier zero has strict NO_RAY semantics.
    if(quantized<settings.cheapThreshold){reflectionCap=0u;refractionCap=0u;rate=2u;}
    else if(quantized<settings.mediumThreshold){reflectionCap=min(reflectionCap,1u);refractionCap=min(refractionCap,1u);rate=max(rate,1u);}
    else if(quantized<settings.highThreshold){reflectionCap=min(reflectionCap,2u);refractionCap=min(refractionCap,2u);}
    // Reserve full-rate shading before estimating ray work because a failed
    // reservation changes the software shading rate and therefore sample count.
    if(rate==0u&&!reserveBudget(2u,64u,budgets[0].maxFullRateSamples)){
        rate=1u;uint ignoredSat=0u;InterlockedOr(budgets[0].budgetSaturated,1u,ignoredSat);
    }
    uint sampleCount=rate==0u?64u:(rate==1u?16u:4u);
    uint reflectionSteps=reflectionCap==3u?40u:(reflectionCap==2u?26u:(reflectionCap==1u?14u:0u));
    uint refractionSteps=refractionCap==3u?32u:(refractionCap==2u?20u:(refractionCap==1u?1u:0u));
    if(reflectionCap>0u&&!reserveBudget(0u,sampleCount*reflectionSteps,budgets[0].maxReflectionWork)){
        reflectionCap=0u;uint ignoredSat=0u;InterlockedOr(budgets[0].budgetSaturated,1u,ignoredSat);
    }
    if(refractionCap>0u&&!reserveBudget(1u,sampleCount*refractionSteps,budgets[0].maxRefractionWork)){
        refractionCap=0u;uint ignoredSat=0u;InterlockedOr(budgets[0].budgetSaturated,1u,ignoredSat);
    }
    uint tier=max(reflectionCap,refractionCap);
    uint dst=0;InterlockedAdd(tierCounts[tier],1u,dst);
    if(dst<settings.maxTiles)tileLists[tier*settings.maxTiles+dst]=packTile(tileIndex,rate,reflectionCap,refractionCap);
}
