import Common.SceneData;
import Water.VirtualWaterShared;

[[vk::binding(1,0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(2,0)]] StructuredBuffer<MaterialData> materials;

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

// Exact bounded reservation for state simulation work. The old InterlockedAdd
// path could make the telemetry counter exceed the configured cap even when the
// page itself was skipped.
bool reserveStateWork(uint work){
    if(work==0u)return true;
    const uint limit=frameBudget[0].maxStateSimulationCells;
    [loop]for(uint attempt=0u;attempt<16u;++attempt){
        const uint current=frameBudget[0].stateSimulationUsed;
        if(current>limit||work>limit-current)return false;
        uint original=0u;
        InterlockedCompareExchange(frameBudget[0].stateSimulationUsed,current,current+work,original);
        if(original==current)return true;
    }
    return false;
}

groupshared uint groupSlot;
groupshared uint groupGeneration;
groupshared uint groupSourceTier;
groupshared uint groupTargetTier;
groupshared uint groupNewAllocation;
groupshared uint groupEventHit;
groupshared uint groupVisible;
groupshared uint groupDoUpdate;
groupshared uint groupSubsteps;
groupshared uint groupMaxFoamBits;
groupshared uint groupMaxVelocityBits;
groupshared uint groupMaxInteractionBits;

GPUWaterStateCell sampleGridRaw(uint slot,uint tier,float2 uv) {
    uv=saturate(uv);
    const uint resolution=waterStateTierResolution(tier);
    float2 p=uv*float(resolution-1u);
    uint2 a=uint2(floor(p));
    uint2 b=min(a+1u,uint2(resolution-1u));
    float2 f=frac(p);
    uint base=slot*WATER_STATE_CELLS_PER_PAGE;
    GPUWaterStateCell s00=stateCellsRead[base+a.y*WATER_STATE_RESOLUTION+a.x];
    GPUWaterStateCell s10=stateCellsRead[base+a.y*WATER_STATE_RESOLUTION+b.x];
    GPUWaterStateCell s01=stateCellsRead[base+b.y*WATER_STATE_RESOLUTION+a.x];
    GPUWaterStateCell s11=stateCellsRead[base+b.y*WATER_STATE_RESOLUTION+b.x];
    GPUWaterStateCell o;
    o.height=lerp(lerp(s00.height,s10.height,f.x),lerp(s01.height,s11.height,f.x),f.y);
    o.velocityX=lerp(lerp(s00.velocityX,s10.velocityX,f.x),lerp(s01.velocityX,s11.velocityX,f.x),f.y);
    o.velocityZ=lerp(lerp(s00.velocityZ,s10.velocityZ,f.x),lerp(s01.velocityZ,s11.velocityZ,f.x),f.y);
    o.foam=lerp(lerp(s00.foam,s10.foam,f.x),lerp(s01.foam,s11.foam,f.x),f.y);
    return o;
}

bool validRefForPage(uint pageId,out GPUWaterStatePhysicalRef ref,out GPUWaterPhysicalState physical) {
    ref=(GPUWaterStatePhysicalRef)0; ref.slot=WATER_INVALID_PHYSICAL_PAGE; physical=(GPUWaterPhysicalState)0;
    if(pageId>=push.pageCount) return false;
    ref=pageTable[pageId];
    if(!waterStateRefValid(ref)) return false;
    physical=physicalStates[ref.slot];
    return physical.generation==ref.generation && physical.generation!=0u;
}

float3 stateWorldPosition(GPUVirtualWaterPage page,RendererInstanceData instance,
                          GPUWaterPhysicalState physical,float2 uv) {
    if((page.flags&WATER_PAGE_SPLINE)!=0u) {
        float2 a=float2(page.splineStartX,page.splineStartZ);
        float2 b=float2(page.splineEndX,page.splineEndZ);
        float2 d=b-a; float len=max(length(d),1e-4); float2 side=float2(-d.y,d.x)/len;
        float halfWidth=lerp(page.splineHalfWidth0,page.splineHalfWidth1,uv.y);
        float2 center=lerp(a,b,uv.y);
        float2 xz=center+side*((uv.x*2.0-1.0)*halfWidth);
        return waterLocalToWorld(float3(xz.x,page.baseHeight,xz.y),instance);
    }
    const float size=waterStateWorldSize(page);
    const float2 localXZ=(float2(physical.ownerX,physical.ownerZ)+uv)*size;
    return waterLocalToWorld(float3(localXZ.x,page.baseHeight,localXZ.y),instance);
}

bool sampleNeighbour(uint pageId,float3 worldPosition,out GPUWaterStateCell state) {
    state=(GPUWaterStateCell)0;
    GPUWaterStatePhysicalRef ref; GPUWaterPhysicalState physical;
    if(!validRefForPage(pageId,ref,physical)) return false;
    GPUVirtualWaterPage page=pages[pageId];
    RendererInstanceData instance=instances[page.instanceIndex];
    float2 uv=waterStateUvFromWorld(page,instance,physical,frame.cameraPosition.xyz,worldPosition);
    if(any(uv<-0.001)||any(uv>1.001)) return false;
    state=sampleGridRaw(ref.slot,physical.tier,uv);
    return true;
}

bool sampleCurrentOrNeighbour(uint pageId,GPUVirtualWaterPage page,RendererInstanceData instance,
                              GPUWaterStatePhysicalRef ref,GPUWaterPhysicalState physical,
                              float3 worldPosition,out GPUWaterStateCell state) {
    float2 uv=waterStateUvFromWorld(page,instance,physical,frame.cameraPosition.xyz,worldPosition);
    if(all(uv>=0.0)&&all(uv<=1.0)) { state=sampleGridRaw(ref.slot,physical.tier,uv); return true; }
    uint neighbour=WATER_INVALID_PHYSICAL_PAGE;
    if(uv.x<0.0) neighbour=uv.y<0.5?page.neighborLeft0:page.neighborLeft1;
    else if(uv.x>1.0) neighbour=uv.y<0.5?page.neighborRight0:page.neighborRight1;
    else if(uv.y<0.0) neighbour=uv.x<0.5?page.neighborBottom0:page.neighborBottom1;
    else if(uv.y>1.0) neighbour=uv.x<0.5?page.neighborTop0:page.neighborTop1;
    return sampleNeighbour(neighbour,worldPosition,state);
}

bool eventTouchesPage(GPUWaterInteractionEvent e,GPUVirtualWaterPage page,RendererInstanceData instance) {
    float2 snapped=waterSnappedOffset(page,instance,frame.cameraPosition.xyz);
    float2 mn=float2(page.minX,page.minZ)+snapped;
    float2 mx=float2(page.maxX,page.maxZ)+snapped;
    if((page.flags&WATER_PAGE_SPLINE)!=0u){mn=float2(page.minX,page.minZ);mx=float2(page.maxX,page.maxZ);}
    float3 center=waterLocalToWorld(float3((mn.x+mx.x)*0.5,page.baseHeight,(mn.y+mx.y)*0.5),instance);
    float2 half=(mx-mn)*max(abs(instance.scaleBase.xz),float2(1e-4))*0.5;
    return length(center.xz-float2(e.worldX,e.worldZ))<=max(e.radius+length(half),1e-3);
}

bool stateCellWet(GPUVirtualWaterPage page,float2 uv) {
    if(page.coverageClass==0u) return false;
    if(page.coverageClass!=2u) return true;
    uint2 cell=min(uint2(saturate(uv)*float2(8.0,8.0)),uint2(7u,7u));
    uint bit=cell.y*8u+cell.x;
    uint word=bit<32u?page.cellCoverageMaskLow:page.cellCoverageMaskHigh;
    return ((word>>(bit&31u))&1u)!=0u;
}

float eventAtPosition(GPUWaterInteractionEvent e,float2 worldXZ) {
    float radius=max(e.radius,1e-3);float x=saturate(1.0-length(worldXZ-float2(e.worldX,e.worldZ))/radius);
    return x*x*(3.0-2.0*x)*e.strength;
}

float gerstnerJacobian(MaterialData material,float2 worldXZ,float time,float cellSize,uint waveMask) {
    float dxx=1.0,dxz=0.0,dzx=0.0,dzz=1.0;
    [unroll] for(uint i=0;i<8;++i){
        if(i>=material.waterWaveCount.x)break;if((waveMask&(1u<<i))==0u)continue;
        float4 w=material.waterWaves[i];float lod=cellSize>0.0?smoothstep(2.0,6.0,w.w/cellSize):1.0;
        if(lod<=0.0)continue;float2 dir=normalize(w.xy);float k=WATER_TWO_PI/max(w.w,1e-3);
        float qak=material.waterWaveMotion[i].y*w.z*k*sin(k*dot(dir,worldXZ)+material.waterWaveMotion[i].x*time)*lod;
        dxx-=qak*dir.x*dir.x;dxz-=qak*dir.x*dir.y;dzx-=qak*dir.x*dir.y;dzz-=qak*dir.y*dir.y;
    }
    return dxx*dzz-dxz*dzx;
}

[numthreads(16,16,1)]
[shader("compute")]
void main(uint3 groupId:SV_GroupID,uint3 localId:SV_GroupThreadID) {
    const uint pageId=groupId.z;
    if(pageId>=push.pageCount)return;
    GPUVirtualWaterPage page=pages[pageId];
    RendererInstanceData instance=instances[page.instanceIndex];
    GPUWaterStatePhysicalRef ref=pageTable[pageId];

    if(localId.x==0u&&localId.y==0u){
        GPUWaterPhysicalState physical=(GPUWaterPhysicalState)0;
        bool valid=waterStateRefValid(ref);
        if(valid){physical=physicalStates[ref.slot];valid=physical.generation==ref.generation&&physical.generation!=0u;}
        groupSlot=valid?ref.slot:WATER_INVALID_PHYSICAL_PAGE;
        groupGeneration=valid?ref.generation:0u;
        groupSourceTier=valid?physical.tier:0u;
        groupTargetTier=valid?min(ref.tier,3u):0u;
        groupNewAllocation=valid&&physical.inactiveSeconds<0.0?1u:0u;
        groupVisible=(history[pageId].packedQuality&WATER_VALID_FLAG)!=0u?1u:0u;
        bool eventHit=false;
        [loop]for(uint e=0u;e<push.eventCount;++e)eventHit=eventHit||eventTouchesPage(events[e],page,instance);
        groupEventHit=eventHit?1u:0u;
        groupMaxFoamBits=0u;groupMaxVelocityBits=0u;groupMaxInteractionBits=0u;
        groupDoUpdate=0u;groupSubsteps=1u;
        if(valid){
            const uint period=waterUpdatePeriod(physical.updateRatePacked);
            const uint phase=waterUpdatePhase(physical.updateRatePacked)%period;
            bool due=((push.frameIndex+period-phase)%period)==0u;
            due=due||eventHit||groupNewAllocation!=0u||physical.tier!=groupTargetTier;
            if(due){
                const uint resolution=waterStateTierResolution(groupTargetTier);
                const float stateSize=waterStateWorldSize(page);
                const float dx=max(stateSize/float(max(resolution,2u)-1u),0.05);
                const float waveSpeed=4.25;
                const float accumulatedDt=max(push.deltaTime,0.0)*float(period);
                const float maxVelocity=max(physical.maxVelocity,0.0);
                const float dtMax=max(0.45*dx/(maxVelocity+waveSpeed+1e-3),1e-4);
                uint substeps=max(1u,uint(ceil(accumulatedDt/dtMax)));
                if(substeps>8u){substeps=8u;physical.updateRatePacked=waterPackUpdateRate(1u,0u);physicalStates[ref.slot]=physical;}
                const uint work=resolution*resolution*substeps;
                // Interaction pages are quality-critical, but still obey the
                // hard frame cap. The scheduler should provision headroom for
                // them rather than allowing unbounded emergency work.
                if(reserveStateWork(work)){groupDoUpdate=1u;groupSubsteps=substeps;}
                else{uint ignored=0u;InterlockedOr(frameBudget[0].budgetSaturated,1u,ignored);}
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if(groupSlot==WATER_INVALID_PHYSICAL_PAGE)return;

    GPUWaterPhysicalState physical=physicalStates[groupSlot];
    if(physical.generation!=groupGeneration)return;
    if(groupDoUpdate==0u){
        if(localId.x==0u&&localId.y==0u){
            physical.inactiveSeconds=(groupVisible!=0u||groupEventHit!=0u)?0.0:max(physical.inactiveSeconds,0.0)+push.deltaTime;
            physicalStates[groupSlot]=physical;
        }
        return;
    }

    const uint resolution=waterStateTierResolution(groupTargetTier);
    const bool inGrid=localId.x<resolution&&localId.y<resolution;
    const float2 stateUv=inGrid?float2(localId.xy)/float(resolution-1u):float2(0.0);
    const bool active=inGrid&&stateCellWet(page,stateUv);
    GPUWaterStateCell state=(GPUWaterStateCell)0;
    float interactionEnergy=0.0;
    float speed=0.0;
    if(inGrid&&!active){
        const uint dryIndex=groupSlot*WATER_STATE_CELLS_PER_PAGE+localId.y*WATER_STATE_RESOLUTION+localId.x;
        stateCellsWrite[dryIndex]=(GPUWaterStateCell)0;
    }
    if(active){
        const float2 uv=stateUv;
        const uint writeIndex=groupSlot*WATER_STATE_CELLS_PER_PAGE+localId.y*WATER_STATE_RESOLUTION+localId.x;
        GPUWaterStateCell previous=(GPUWaterStateCell)0;
        if(groupNewAllocation==0u) previous=sampleGridRaw(groupSlot,groupSourceTier,uv);
        float3 worldPosition=stateWorldPosition(page,instance,physical,uv);

        float2 authoredFlow=0.0;
        if(page.flowSpeed>1e-4){float3 wf=waterRotateQuaternion(float3(page.flowX,0,page.flowZ),instance.rotation);float len=length(wf.xz);if(len>1e-5)authoredFlow=wf.xz/len*page.flowSpeed;}
        if(groupNewAllocation==0u&&length(authoredFlow)>1e-5){
            GPUWaterStateCell advectedPrevious=(GPUWaterStateCell)0;float3 back=worldPosition;back.xz-=authoredFlow*max(push.deltaTime,0.0);
            if(sampleCurrentOrNeighbour(pageId,page,instance,ref,physical,back,advectedPrevious))previous=advectedPrevious;
        }

        const float stateSize=waterStateWorldSize(page);
        const float dx=max(stateSize/float(resolution-1u),0.05);
        float3 leftPos=worldPosition,rightPos=worldPosition,bottomPos=worldPosition,topPos=worldPosition;
        float3 localX=waterRotateQuaternion(float3(dx,0,0),instance.rotation);
        float3 localZ=waterRotateQuaternion(float3(0,0,dx),instance.rotation);
        leftPos-=localX;rightPos+=localX;bottomPos-=localZ;topPos+=localZ;
        GPUWaterStateCell left=previous,right=previous,bottom=previous,top=previous,tmp;
        if(groupNewAllocation==0u){
            if(sampleCurrentOrNeighbour(pageId,page,instance,ref,physical,leftPos,tmp))left=tmp;
            if(sampleCurrentOrNeighbour(pageId,page,instance,ref,physical,rightPos,tmp))right=tmp;
            if(sampleCurrentOrNeighbour(pageId,page,instance,ref,physical,bottomPos,tmp))bottom=tmp;
            if(sampleCurrentOrNeighbour(pageId,page,instance,ref,physical,topPos,tmp))top=tmp;
        }
        float impulse=0.0;[loop]for(uint e=0u;e<push.eventCount;++e)impulse+=eventAtPosition(events[e],worldPosition.xz);

        state=previous;
        const uint period=waterUpdatePeriod(physical.updateRatePacked);
        float totalDt=max(push.deltaTime,0.0)*float(period);
        const float dt=totalDt/float(max(groupSubsteps,1u));
        const float gravity=18.0625; // 4.25^2, linearized shallow-water c^2
        [loop]for(uint step=0u;step<groupSubsteps;++step){
            float gradX=(right.height-left.height)/(2.0*dx);
            float gradZ=(top.height-bottom.height)/(2.0*dx);
            float divV=((right.velocityX-left.velocityX)+(top.velocityZ-bottom.velocityZ))/(2.0*dx);
            state.velocityX+=(-gravity*gradX+authoredFlow.x*0.08)*dt;
            state.velocityZ+=(-gravity*gradZ+authoredFlow.y*0.08)*dt;
            state.height+=(-divV+impulse*2.0)*dt;
            state.velocityX*=exp(-1.9*dt);state.velocityZ*=exp(-1.9*dt);state.height*=exp(-0.35*dt);
            state.height=clamp(state.height,-0.75,0.75);
        }

        MaterialData material=materials[asuint(instance.positionMaterial.w)];
        float jacobian=gerstnerJacobian(material,worldPosition.xz,frame.windGustFrequencyTime.z,
                                        page.cellSize,page.waveCandidateMask);
        float crest=saturate((material.waterFoam.y-jacobian)*3.0);
        float2 flow=length(authoredFlow)>1e-5?authoredFlow:frame.windDirectionStrength.xz;
        float flowLen=length(flow);flow=(flowLen>1e-5?flow/flowLen:float2(1,0))*(0.55+0.15*frame.windDirectionStrength.w);
        float3 back=worldPosition;back.xz-=flow*totalDt;
        GPUWaterStateCell advected=(GPUWaterStateCell)0;float sourceFoam=previous.foam;
        if(groupNewAllocation==0u&&sampleCurrentOrNeighbour(pageId,page,instance,ref,physical,back,advected))sourceFoam=advected.foam;
        speed=length(float2(state.velocityX,state.velocityZ));
        state.foam=saturate(max(sourceFoam*exp(-0.45*totalDt),crest)+abs(impulse)*0.42+speed*0.008);
        stateCellsWrite[writeIndex]=state;
        interactionEnergy=abs(state.height)+speed*0.12;
        uint ignored=0u;InterlockedMax(groupMaxFoamBits,asuint(max(state.foam,0.0)),ignored);
        InterlockedMax(groupMaxVelocityBits,asuint(max(speed,0.0)),ignored);
        InterlockedMax(groupMaxInteractionBits,asuint(max(interactionEnergy,0.0)),ignored);
    }
    GroupMemoryBarrierWithGroupSync();

    if(localId.x==0u&&localId.y==0u&&physicalStates[groupSlot].generation==groupGeneration){
        physical=physicalStates[groupSlot];
        physical.foam=asfloat(groupMaxFoamBits);physical.maxVelocity=asfloat(groupMaxVelocityBits);
        physical.interaction=asfloat(groupMaxInteractionBits);physical.inactiveSeconds=(groupVisible!=0u||groupEventHit!=0u)?0.0:max(physical.inactiveSeconds,0.0)+push.deltaTime;
        physical.tier=groupTargetTier;physical.lastSimulatedFrame=push.frameIndex;
        if(groupEventHit!=0u)physical.lastInteractionFrame=push.frameIndex;
        physicalStates[groupSlot]=physical;
    }
}
