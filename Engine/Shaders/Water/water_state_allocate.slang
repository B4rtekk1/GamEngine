import Common.SceneData;
import Water.VirtualWaterShared;

[[vk::binding(1,0)]] ConstantBuffer<RendererFrameData> frame;

[[vk::binding(0,1)]] StructuredBuffer<GPUVirtualWaterPage> pages;
[[vk::binding(1,1)]] StructuredBuffer<RendererInstanceData> instances;
[[vk::binding(2,1)]] StructuredBuffer<GPUWaterPageHistory> history;
[[vk::binding(3,1)]] globallycoherent RWStructuredBuffer<GPUWaterStatePhysicalRef> pageTable;
[[vk::binding(4,1)]] globallycoherent RWStructuredBuffer<GPUWaterPhysicalState> physicalStates;
[[vk::binding(5,1)]] StructuredBuffer<GPUWaterInteractionEvent> events;
[[vk::binding(6,1)]] RWStructuredBuffer<uint> allocatorCounter;
[[vk::binding(7,1)]] StructuredBuffer<GPUWaterStateCell> stateCellsRead;
[[vk::binding(8,1)]] RWStructuredBuffer<GPUWaterStateCell> stateCellsWrite;
[[vk::binding(9,1)]] RWStructuredBuffer<GPUWaterFrameBudget> frameBudget;

// frameCounter is the monotonically increasing water frame number.
struct Push {
    uint eventCount, pageCount, frameCounter, pad;
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

static const uint ALLOCATOR_THREADS = WATER_MAX_PHYSICAL_STATE_PAGES;
static const uint MAX_ALLOCATIONS_PER_FRAME = 256u;
static const uint REQUEST_EVENT_HIT = 1u << 16u;
static const uint REQUEST_VISIBLE = 1u << 17u;
static const uint REQUEST_TIER_SHIFT = 18u;

groupshared uint requestList[MAX_ALLOCATIONS_PER_FRAME];
groupshared uint requestCount;
groupshared uint bestVictimKey;
groupshared uint sharedCursor;

bool pageEventHit(GPUVirtualWaterPage page, RendererInstanceData instance) {
    bool hit=false;
    [loop] for(uint eventIndex=0u;eventIndex<push.eventCount;++eventIndex)
        hit = hit || eventTouchesPage(events[eventIndex],page,instance);
    return hit;
}

GPUWaterStatePhysicalRef invalidRef() {
    GPUWaterStatePhysicalRef ref=(GPUWaterStatePhysicalRef)0;
    ref.slot=WATER_INVALID_PHYSICAL_PAGE;
    return ref;
}

// Victim key for the parallel slot search: free slots beat every evictable
// slot, higher LRU score beats lower, and ties prefer the slot closest after
// the rotating cursor. Zero means "not evictable". Slots that are visible,
// pinned or were allocated this frame (inactiveSeconds < 0) are never
// evicted, so a full cache degrades to "no state" instead of thrashing.
uint victimKey(uint slot, uint cursor) {
    const GPUWaterPhysicalState physical=physicalStates[slot];
    const uint rotation=(slot+WATER_MAX_PHYSICAL_STATE_PAGES-cursor)%WATER_MAX_PHYSICAL_STATE_PAGES;
    const uint order=255u-rotation;
    if(physical.generation==0u) return 0xffffff00u|order;
    if(physical.pinCount!=0u || physical.inactiveSeconds<0.0 || physical.lastVisibleFrame==push.frameCounter)
        return 0u;
    const float score=physical.inactiveSeconds+(push.frameCounter-physical.lastInteractionFrame>8u?2.0:0.0);
    const uint quantized=min(uint(clamp(score+1.0,0.0,65000.0)*256.0),0xfffffdu)+1u;
    return (quantized<<8u)|order;
}

// Multi-rate neighbour normalization. The state update samples across page
// borders; limiting adjacent periods to a 2:1 ratio prevents a fast page from
// repeatedly exchanging state with an effectively frozen neighbour. Every page
// only ever lowers its own period (each physical slot has exactly one owner),
// so the parallel relaxation is race-free and monotone; four passes cover the
// supported {1,2,4,8} periods.
uint neighbourPeriodLimit(uint neighbourPage, uint limit) {
    if(neighbourPage>=push.pageCount) return limit;
    const GPUWaterStatePhysicalRef ref=pageTable[neighbourPage];
    if(!waterStateRefValid(ref)) return limit;
    const GPUWaterPhysicalState physical=physicalStates[ref.slot];
    if(physical.generation!=ref.generation||physical.generation==0u) return limit;
    return min(limit,waterUpdatePeriod(physical.updateRatePacked)*2u);
}

void tightenPagePeriod(uint pageId) {
    const GPUWaterStatePhysicalRef ref=pageTable[pageId];
    if(!waterStateRefValid(ref)) return;
    GPUWaterPhysicalState physical=physicalStates[ref.slot];
    if(physical.generation!=ref.generation||physical.generation==0u) return;
    const GPUVirtualWaterPage p=pages[pageId];
    const uint current=waterUpdatePeriod(physical.updateRatePacked);
    uint limit=current;
    limit=neighbourPeriodLimit(p.neighborLeft0,limit);   limit=neighbourPeriodLimit(p.neighborLeft1,limit);
    limit=neighbourPeriodLimit(p.neighborRight0,limit);  limit=neighbourPeriodLimit(p.neighborRight1,limit);
    limit=neighbourPeriodLimit(p.neighborBottom0,limit); limit=neighbourPeriodLimit(p.neighborBottom1,limit);
    limit=neighbourPeriodLimit(p.neighborTop0,limit);    limit=neighbourPeriodLimit(p.neighborTop1,limit);
    if(limit>=current) return;
    physical.updateRatePacked=waterPackUpdateRate(limit,waterUpdatePhase(physical.updateRatePacked)%limit);
    physicalStates[ref.slot]=physical;
}

// One workgroup: phase 1 validates/reconnects every page in parallel, phase 2
// serially grants new slots (with a parallel victim search per request) so two
// pages can never claim the same slot, phase 3 relaxes update rates.
[numthreads(ALLOCATOR_THREADS,1,1)]
[shader("compute")]
void main(uint3 localId:SV_GroupThreadID) {
    const uint lane=localId.x;
    if(lane==0u){ requestCount=0u; sharedCursor=allocatorCounter[0]%WATER_MAX_PHYSICAL_STATE_PAGES; }
    GroupMemoryBarrierWithGroupSync();

    [loop] for (uint pageId=lane; pageId<push.pageCount; pageId+=ALLOCATOR_THREADS) {
        GPUVirtualWaterPage page=pages[pageId];
        if ((page.executionFlags & WATER_EXEC_SPARSE_STATE) == 0u) {
            pageTable[pageId]=invalidRef();
            continue;
        }
        RendererInstanceData instance=instances[page.instanceIndex];
        GPUWaterPageHistory h=history[pageId];
        const bool visible=(h.packedQuality&WATER_VALID_FLAG)!=0u;
        const bool eventHit=pageEventHit(page,instance);
        const bool stateOnly=(page.flags&WATER_PAGE_STATE_ONLY)!=0u;
        const int2 stateCoord=waterStateWorldCoord(page,instance,frame.cameraPosition.xyz);
        GPUWaterStatePhysicalRef ref=pageTable[pageId];
        bool refValid=false;
        if(waterStateRefValid(ref)) {
            GPUWaterPhysicalState physical=physicalStates[ref.slot];
            refValid=physical.generation==ref.generation && ownerMatches(physical,page,stateCoord);
        }

        // Reconnect a recentered geometry slot to an already-resident world state page.
        if(!refValid) {
            ref=invalidRef();
            [loop] for(uint slot=0u;slot<WATER_MAX_PHYSICAL_STATE_PAGES;++slot) {
                GPUWaterPhysicalState physical=physicalStates[slot];
                if(ownerMatches(physical,page,stateCoord)) {
                    ref.slot=slot; ref.generation=physical.generation; ref.tier=physical.tier;
                    refValid=true; break;
                }
            }
        }

        const uint desiredTier=desiredTierForPage(page,h,eventHit);
        if(!refValid) {
            pageTable[pageId]=ref;
            if(stateOnly||eventHit||visible) {
                uint request=0u; InterlockedAdd(requestCount,1u,request);
                if(request<MAX_ALLOCATIONS_PER_FRAME)
                    requestList[request]=pageId|(eventHit?REQUEST_EVENT_HIT:0u)|(visible?REQUEST_VISIBLE:0u)|
                                         (desiredTier<<REQUEST_TIER_SHIFT);
            }
            continue;
        }

        GPUWaterPhysicalState physical=physicalStates[ref.slot];
        ref.tier=desiredTier;
        if(visible) physical.lastVisibleFrame=push.frameCounter;
        if(eventHit) physical.lastInteractionFrame=push.frameCounter;
        uint period=desiredPeriod(desiredTier,visible,eventHit);
        uint phase=waterUpdatePhase(physical.updateRatePacked)%period;
        physical.updateRatePacked=waterPackUpdateRate(period,phase);
        physical.pinCount=eventHit?2u:(physical.pinCount>0u?physical.pinCount-1u:0u);
        // Keep physical.tier as the source resolution until the update pass
        // has resampled into the requested ref.tier.
        physicalStates[ref.slot]=physical;
        pageTable[pageId]=ref;
    }
    AllMemoryBarrierWithGroupSync();

    const uint requests=min(requestCount,MAX_ALLOCATIONS_PER_FRAME);
    [loop] for(uint requestIndex=0u; requestIndex<requests; ++requestIndex) {
        if(lane==0u) bestVictimKey=0u;
        GroupMemoryBarrierWithGroupSync();
        const uint cursor=sharedCursor;
        uint ignored=0u; InterlockedMax(bestVictimKey,victimKey(lane,cursor),ignored);
        GroupMemoryBarrierWithGroupSync();
        if(lane==0u && bestVictimKey!=0u) {
            const uint request=requestList[requestIndex];
            const uint pageId=request&0xffffu;
            const bool eventHit=(request&REQUEST_EVENT_HIT)!=0u;
            const bool visible=(request&REQUEST_VISIBLE)!=0u;
            const uint desiredTier=(request>>REQUEST_TIER_SHIFT)&3u;
            const uint chosen=(cursor+(255u-(bestVictimKey&0xffu)))%WATER_MAX_PHYSICAL_STATE_PAGES;
            GPUVirtualWaterPage page=pages[pageId];
            RendererInstanceData instance=instances[page.instanceIndex];
            const int2 stateCoord=waterStateWorldCoord(page,instance,frame.cameraPosition.xyz);

            uint generation=physicalStates[chosen].generation+1u;
            if(generation==0u) generation=1u;
            GPUWaterPhysicalState fresh=(GPUWaterPhysicalState)0;
            fresh.inactiveSeconds=-1.0;
            fresh.ownerBodyIndex=page.bodyIdIndex;
            fresh.ownerBodyGeneration=page.bodyIdGeneration;
            fresh.ownerX=stateCoord.x; fresh.ownerZ=stateCoord.y;
            fresh.generation=generation; fresh.tier=desiredTier;
            fresh.pinCount=eventHit?2u:0u;
            fresh.lastVisibleFrame=visible?push.frameCounter:0u;
            fresh.lastInteractionFrame=eventHit?push.frameCounter:0u;
            uint period=desiredPeriod(desiredTier,visible,eventHit);
            uint phase=(uint(stateCoord.x)*73856093u^uint(stateCoord.y)*19349663u^page.bodyIdIndex)%period;
            fresh.updateRatePacked=waterPackUpdateRate(period,phase);
            physicalStates[chosen]=fresh;
            GPUWaterStatePhysicalRef ref=invalidRef();
            ref.tier=desiredTier; ref.slot=chosen; ref.generation=generation;
            pageTable[pageId]=ref;
            sharedCursor=(chosen+1u)%WATER_MAX_PHYSICAL_STATE_PAGES;
        }
        AllMemoryBarrierWithGroupSync();
    }

    [unroll] for(uint relaxation=0u;relaxation<4u;++relaxation){
        [loop] for(uint pageId=lane;pageId<push.pageCount;pageId+=ALLOCATOR_THREADS)
            tightenPagePeriod(pageId);
        AllMemoryBarrierWithGroupSync();
    }
    if(lane==0u) allocatorCounter[0]=sharedCursor;
}
