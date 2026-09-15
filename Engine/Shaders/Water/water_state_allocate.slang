import Common.SceneData;

[[vk::binding(1,0)]] ConstantBuffer<RendererFrameData> frame;

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
struct GPUWaterPageHistory { uint packedQuality, visibleAge; float previousImportance, reserved; };
struct GPUWaterPhysicalState { float foam, interaction, interactionVelocity, inactiveSeconds; };
struct GPUWaterInteractionEvent { float worldX, worldZ, radius, strength; };
struct GPUWaterStateCell { float height, velocity, foam, reserved; };

[[vk::binding(0,1)]] StructuredBuffer<GPUVirtualWaterPage> pages;
[[vk::binding(1,1)]] StructuredBuffer<RendererInstanceData> instances;
[[vk::binding(2,1)]] StructuredBuffer<GPUWaterPageHistory> history;
[[vk::binding(3,1)]] RWStructuredBuffer<uint> pageTable;
[[vk::binding(4,1)]] RWStructuredBuffer<uint> physicalOwners;
[[vk::binding(5,1)]] RWStructuredBuffer<GPUWaterPhysicalState> physicalStates;
[[vk::binding(6,1)]] StructuredBuffer<GPUWaterInteractionEvent> events;
[[vk::binding(7,1)]] RWStructuredBuffer<uint> allocatorCounter;
[[vk::binding(8,1)]] StructuredBuffer<GPUWaterStateCell> stateCellsRead;
[[vk::binding(9,1)]] RWStructuredBuffer<GPUWaterStateCell> stateCellsWrite;

struct Push {
    uint eventCount, pageCount, frameIndex, pad;
    float deltaTime, pad1, pad2, pad3;
};
[[vk::push_constant]] ConstantBuffer<Push> push;

static const uint InvalidPhysicalPage = 0xffffffffu;
static const uint MaxPhysicalStatePages = 256u;

float3 rotateQuaternion(float3 p,float4 q){return p+2.0*cross(q.xyz,cross(q.xyz,p)+q.w*p);}
float3 localToWorld(float3 p,RendererInstanceData i){return rotateQuaternion(p*i.scaleBase.xyz,i.rotation)+i.positionMaterial.xyz;}

bool eventTouchesPage(GPUWaterInteractionEvent e,GPUVirtualWaterPage p,RendererInstanceData instance){
    float3 localCenter=float3((p.minX+p.maxX)*0.5,p.baseHeight,(p.minZ+p.maxZ)*0.5);
    float3 center=localToWorld(localCenter,instance);
    float2 halfExtent=float2((p.maxX-p.minX)*abs(instance.scaleBase.x),(p.maxZ-p.minZ)*abs(instance.scaleBase.z))*0.5;
    return length(center.xz-float2(e.worldX,e.worldZ))<=max(e.radius+length(halfExtent),1e-3);
}

// One invocation owns allocation/eviction for the complete bounded cache.
// This deliberately serialises only the tiny page-table mutation stage. The
// expensive 16x16 state solve remains fully parallel in water_state_update.
[numthreads(1,1,1)]
[shader("compute")]
void main(uint3 tid:SV_DispatchThreadID){
    if(any(tid!=0)) return;
    uint cursor=allocatorCounter[0];
    [loop] for(uint pageId=0;pageId<push.pageCount;++pageId){
        GPUVirtualWaterPage page=pages[pageId];
        RendererInstanceData instance=instances[page.instanceIndex];
        uint quality=history[pageId].packedQuality;
        bool visible=(quality&0x80000000u)!=0u;
        uint foamTier=(quality>>4u)&3u;
        bool eventHit=false;
        [loop] for(uint eventIndex=0;eventIndex<push.eventCount;++eventIndex)
            eventHit=eventHit||eventTouchesPage(events[eventIndex],page,instance);

        uint physical=pageTable[pageId];
        if(physical!=InvalidPhysicalPage &&
           (physical>=MaxPhysicalStatePages || physicalOwners[physical]!=pageId)){
            pageTable[pageId]=InvalidPhysicalPage;
            physical=InvalidPhysicalPage;
        }

        const bool stateOnly=(page.flags&1u)==0u;
        bool needsState=stateOnly||eventHit||(visible&&foamTier!=0u)||physical!=InvalidPhysicalPage;
        if(!needsState) continue;
        if(physical!=InvalidPhysicalPage) continue;

        // Prefer an actually free slot before evicting the ring cursor.
        uint chosen=InvalidPhysicalPage;
        [loop] for(uint attempt=0;attempt<MaxPhysicalStatePages;++attempt){
            uint candidate=(cursor+attempt)%MaxPhysicalStatePages;
            if(physicalOwners[candidate]==InvalidPhysicalPage){chosen=candidate;cursor=candidate+1u;break;}
        }
        if(chosen==InvalidPhysicalPage){chosen=cursor%MaxPhysicalStatePages;cursor=chosen+1u;}

        uint previousOwner=physicalOwners[chosen];
        if(previousOwner!=InvalidPhysicalPage && previousOwner<push.pageCount && pageTable[previousOwner]==chosen)
            pageTable[previousOwner]=InvalidPhysicalPage;

        physicalOwners[chosen]=pageId;
        pageTable[pageId]=chosen;
        // Negative inactiveSeconds is a generation marker. The solver treats
        // the old physical texels as zero on the first update after reuse.
        GPUWaterPhysicalState fresh={0.0,0.0,0.0,-1.0};
        physicalStates[chosen]=fresh;
    }
    allocatorCounter[0]=cursor;
}
