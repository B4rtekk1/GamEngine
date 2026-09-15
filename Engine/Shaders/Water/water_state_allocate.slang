import Common.SceneData;
import Water.VirtualWaterShared;

[[vk::binding(1,0)]] ConstantBuffer<RendererFrameData> frame;

[[vk::binding(0,1)]] StructuredBuffer<GPUVirtualWaterPage> pages;
[[vk::binding(1,1)]] StructuredBuffer<RendererInstanceData> instances;
[[vk::binding(2,1)]] StructuredBuffer<GPUWaterPageHistory> history;
[[vk::binding(3,1)]] RWStructuredBuffer<GPUWaterStatePhysicalRef> pageTable;
[[vk::binding(4,1)]] RWStructuredBuffer<GPUWaterPhysicalState> physicalStates;
[[vk::binding(5,1)]] StructuredBuffer<GPUWaterInteractionEvent> events;
[[vk::binding(6,1)]] RWStructuredBuffer<uint> allocatorCounter;
[[vk::binding(7,1)]] StructuredBuffer<GPUWaterStateCell> stateCellsRead;
[[vk::binding(8,1)]] RWStructuredBuffer<GPUWaterStateCell> stateCellsWrite;
[[vk::binding(9,1)]] RWStructuredBuffer<GPUWaterFrameBudget> frameBudget;

struct Push {
    uint eventCount, pageCount, frameIndex, pad;
    float deltaTime, pad1, pad2, pad3;
};
[[vk::push_constant]] ConstantBuffer<Push> push;

bool eventTouchesPage(GPUWaterInteractionEvent e, GPUVirtualWaterPage page, RendererInstanceData instance) {
    const float2 snapped = waterSnappedOffset(page, instance, frame.cameraPosition.xyz);
    float2 minXZ = float2(page.minX, page.minZ) + snapped;
    float2 maxXZ = float2(page.maxX, page.maxZ) + snapped;
    if ((page.flags & WATER_PAGE_SPLINE) != 0u) {
        minXZ = float2(page.minX, page.minZ);
        maxXZ = float2(page.maxX, page.maxZ);
    }
    const float3 center = waterLocalToWorld(float3((minXZ.x + maxXZ.x) * 0.5, page.baseHeight,
                                                   (minXZ.y + maxXZ.y) * 0.5), instance);
    const float2 halfExtent = float2(maxXZ.x-minXZ.x,maxXZ.y-minXZ.y) *
                              max(abs(instance.scaleBase.xz),float2(1e-4)) * 0.5;
    return length(center.xz - float2(e.worldX,e.worldZ)) <= max(e.radius + length(halfExtent),1e-3);
}

bool ownerMatches(GPUWaterPhysicalState physical, GPUVirtualWaterPage page, int2 stateCoord) {
    return physical.generation != 0u &&
           physical.ownerBodyIndex == page.bodyIdIndex &&
           physical.ownerBodyGeneration == page.bodyIdGeneration &&
           physical.ownerX == stateCoord.x && physical.ownerZ == stateCoord.y;
}

uint desiredTierForPage(GPUVirtualWaterPage page, GPUWaterPageHistory h, bool eventHit) {
    if (eventHit) return 3u;
    const uint reflection = h.packedQuality & 3u;
    const uint refraction = (h.packedQuality >> 2u) & 3u;
    const uint foam = (h.packedQuality >> 4u) & 3u;
    const uint quality = max(reflection,max(refraction,foam));
    if (quality >= 3u) return 3u;
    if (quality >= 2u || (page.flags & WATER_PAGE_SHORELINE) != 0u) return 2u;
    if ((h.packedQuality & WATER_VALID_FLAG) != 0u || (page.flags & WATER_PAGE_STATE_ONLY) != 0u) return 1u;
    return 0u;
}

uint desiredPeriod(uint tier, bool visible, bool eventHit) {
    if (eventHit || tier >= 3u) return 1u;
    if (visible || tier == 2u) return 2u;
    if (tier == 1u) return 4u;
    return 8u;
}

// Multi-rate neighbour normalization.  The state update samples across page
// borders; limiting adjacent periods to a 2:1 ratio prevents a fast page from
// repeatedly exchanging state with an effectively frozen neighbour.  Four
// serial relaxation passes are enough for the supported {1,2,4,8} periods.
void tightenPeriodPair(uint aPage,uint bPage) {
    if(aPage>=push.pageCount||bPage>=push.pageCount||aPage==bPage)return;
    GPUWaterStatePhysicalRef aRef=pageTable[aPage],bRef=pageTable[bPage];
    if(!waterStateRefValid(aRef)||!waterStateRefValid(bRef))return;
    GPUWaterPhysicalState a=physicalStates[aRef.slot],b=physicalStates[bRef.slot];
    if(a.generation!=aRef.generation||b.generation!=bRef.generation||
       a.generation==0u||b.generation==0u)return;
    uint ap=waterUpdatePeriod(a.updateRatePacked),bp=waterUpdatePeriod(b.updateRatePacked);
    if(ap>bp*2u){
        ap=min(bp*2u,8u);
        a.updateRatePacked=waterPackUpdateRate(ap,waterUpdatePhase(a.updateRatePacked)%ap);
        physicalStates[aRef.slot]=a;
    } else if(bp>ap*2u){
        bp=min(ap*2u,8u);
        b.updateRatePacked=waterPackUpdateRate(bp,waterUpdatePhase(b.updateRatePacked)%bp);
        physicalStates[bRef.slot]=b;
    }
}

[numthreads(1,1,1)]
[shader("compute")]
void main(uint3 tid:SV_DispatchThreadID) {
    if (any(tid != 0u)) return;
    uint cursor = allocatorCounter[0];

    [loop] for (uint pageId=0u; pageId<push.pageCount; ++pageId) {
        GPUVirtualWaterPage page=pages[pageId];
        if ((page.executionFlags & WATER_EXEC_SPARSE_STATE) == 0u) {
            pageTable[pageId]=(GPUWaterStatePhysicalRef)0;
            pageTable[pageId].slot=WATER_INVALID_PHYSICAL_PAGE;
            continue;
        }
        RendererInstanceData instance=instances[page.instanceIndex];
        GPUWaterPageHistory h=history[pageId];
        const bool visible=(h.packedQuality&WATER_VALID_FLAG)!=0u;
        bool eventHit=false;
        [loop] for(uint eventIndex=0u;eventIndex<push.eventCount;++eventIndex)
            eventHit = eventHit || eventTouchesPage(events[eventIndex],page,instance);

        const bool stateOnly=(page.flags&WATER_PAGE_STATE_ONLY)!=0u;
        const int2 stateCoord=waterStateWorldCoord(page,instance,frame.cameraPosition.xyz);
        GPUWaterStatePhysicalRef ref=pageTable[pageId];
        bool refValid=false;
        if(waterStateRefValid(ref)) {
            GPUWaterPhysicalState physical=physicalStates[ref.slot];
            refValid=physical.generation==ref.generation && ownerMatches(physical,page,stateCoord);
        }
        if(!refValid) {
            ref.slot=WATER_INVALID_PHYSICAL_PAGE; ref.generation=0u; ref.tier=0u; ref.reserved=0u;
            pageTable[pageId]=ref;
        }

        // Reconnect a recentered geometry slot to an already-resident world state page.
        if(!refValid) {
            [loop] for(uint slot=0u;slot<WATER_MAX_PHYSICAL_STATE_PAGES;++slot) {
                GPUWaterPhysicalState physical=physicalStates[slot];
                if(ownerMatches(physical,page,stateCoord)) {
                    ref.slot=slot; ref.generation=physical.generation; ref.tier=physical.tier; ref.reserved=0u;
                    refValid=true; break;
                }
            }
        }

        const uint desiredTier=desiredTierForPage(page,h,eventHit);
        const bool needsState=stateOnly||eventHit||visible||refValid;
        if(!needsState) { pageTable[pageId]=ref; continue; }

        if(!refValid) {
            uint chosen=WATER_INVALID_PHYSICAL_PAGE;
            [loop] for(uint attempt=0u;attempt<WATER_MAX_PHYSICAL_STATE_PAGES;++attempt) {
                uint candidate=(cursor+attempt)%WATER_MAX_PHYSICAL_STATE_PAGES;
                if(physicalStates[candidate].generation==0u) { chosen=candidate; break; }
            }
            if(chosen==WATER_INVALID_PHYSICAL_PAGE) {
                float oldest=-1.0;
                [loop] for(uint attempt=0u;attempt<WATER_MAX_PHYSICAL_STATE_PAGES;++attempt) {
                    uint candidate=(cursor+attempt)%WATER_MAX_PHYSICAL_STATE_PAGES;
                    GPUWaterPhysicalState physical=physicalStates[candidate];
                    if(physical.pinCount!=0u) continue;
                    const float score=physical.inactiveSeconds +
                        (push.frameIndex-physical.lastInteractionFrame>8u?2.0:0.0);
                    if(score>oldest){oldest=score;chosen=candidate;}
                }
            }
            if(chosen==WATER_INVALID_PHYSICAL_PAGE) continue;
            cursor=(chosen+1u)%WATER_MAX_PHYSICAL_STATE_PAGES;

            GPUWaterPhysicalState previous=physicalStates[chosen];
            uint generation=previous.generation+1u;
            if(generation==0u) generation=1u;
            GPUWaterPhysicalState fresh=(GPUWaterPhysicalState)0;
            fresh.inactiveSeconds=-1.0;
            fresh.ownerBodyIndex=page.bodyIdIndex;
            fresh.ownerBodyGeneration=page.bodyIdGeneration;
            fresh.ownerX=stateCoord.x; fresh.ownerZ=stateCoord.y;
            fresh.generation=generation; fresh.tier=desiredTier;
            fresh.pinCount=eventHit?2u:0u;
            fresh.lastVisibleFrame=visible?push.frameIndex:0u;
            fresh.lastInteractionFrame=eventHit?push.frameIndex:0u;
            uint period=desiredPeriod(desiredTier,visible,eventHit);
            uint phase=(uint(stateCoord.x)*73856093u^uint(stateCoord.y)*19349663u^page.bodyIdIndex)%period;
            fresh.updateRatePacked=waterPackUpdateRate(period,phase);
            physicalStates[chosen]=fresh;
            ref.tier=desiredTier; ref.slot=chosen; ref.generation=generation; ref.reserved=0u;
        } else {
            GPUWaterPhysicalState physical=physicalStates[ref.slot];
            ref.tier=desiredTier;
            if(visible) physical.lastVisibleFrame=push.frameIndex;
            if(eventHit) physical.lastInteractionFrame=push.frameIndex;
            uint period=desiredPeriod(desiredTier,visible,eventHit);
            uint phase=waterUpdatePhase(physical.updateRatePacked)%period;
            physical.updateRatePacked=waterPackUpdateRate(period,phase);
            physical.pinCount=eventHit?2u:(physical.pinCount>0u?physical.pinCount-1u:0u);
            // Keep physical.tier as the source resolution until the update pass
            // has resampled into the requested ref.tier.
            physicalStates[ref.slot]=physical;
        }
        pageTable[pageId]=ref;
    }
    // Conservative mixed-rate boundary policy.  This is deliberately done in
    // the serial allocation pass so no extra synchronization pass is required.
    [unroll]for(uint relaxation=0u;relaxation<4u;++relaxation){
        [loop]for(uint pageId=0u;pageId<push.pageCount;++pageId){
            GPUVirtualWaterPage p=pages[pageId];
            tightenPeriodPair(pageId,p.neighborLeft0);   tightenPeriodPair(pageId,p.neighborLeft1);
            tightenPeriodPair(pageId,p.neighborRight0);  tightenPeriodPair(pageId,p.neighborRight1);
            tightenPeriodPair(pageId,p.neighborBottom0); tightenPeriodPair(pageId,p.neighborBottom1);
            tightenPeriodPair(pageId,p.neighborTop0);    tightenPeriodPair(pageId,p.neighborTop1);
        }
    }
    allocatorCounter[0]=cursor;
}
