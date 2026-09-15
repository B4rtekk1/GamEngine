import Common.SceneData;

[[vk::binding(1,0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(2,0)]] StructuredBuffer<MaterialData> materials;

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
struct GPUWaterStateCell { float height, velocity, foam, reserved; };
struct GPUWaterInteractionEvent { float worldX, worldZ, radius, strength; };

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
static const uint StateResolution = 16u;
static const uint StateCellsPerPage = StateResolution * StateResolution;
static const float TwoPi = 6.28318530718;

groupshared uint groupPhysical;
groupshared uint groupNewAllocation;
groupshared uint groupVisible;
groupshared uint groupFoamTier;
groupshared uint groupEventHit;
groupshared uint groupMaxFoamBits;
groupshared uint groupMaxInteractionBits;

float3 rotateQuaternion(float3 p, float4 q) {
    return p + 2.0 * cross(q.xyz, cross(q.xyz, p) + q.w * p);
}
float3 inverseRotateQuaternion(float3 p, float4 q) {
    return p + 2.0 * cross(-q.xyz, cross(-q.xyz, p) + q.w * p);
}
float3 localToWorld(float3 p, RendererInstanceData i) {
    return rotateQuaternion(p * i.scaleBase.xyz, i.rotation) + i.positionMaterial.xyz;
}
float2 worldToPageUv(GPUVirtualWaterPage p, RendererInstanceData i, float3 worldPosition) {
    float3 local = inverseRotateQuaternion(worldPosition - i.positionMaterial.xyz, i.rotation);
    local /= max(abs(i.scaleBase.xyz), float3(1e-5));
    return (local.xz - float2(p.minX, p.minZ)) /
           max(float2(p.maxX-p.minX, p.maxZ-p.minZ), float2(1e-5));
}

bool eventTouchesPage(GPUWaterInteractionEvent e, GPUVirtualWaterPage p, RendererInstanceData instance) {
    float3 localCenter = float3((p.minX + p.maxX) * 0.5, p.baseHeight,
                                (p.minZ + p.maxZ) * 0.5);
    float3 center = localToWorld(localCenter, instance);
    float2 halfExtent = float2((p.maxX - p.minX) * abs(instance.scaleBase.x),
                               (p.maxZ - p.minZ) * abs(instance.scaleBase.z)) * 0.5;
    float pageRadius = length(halfExtent);
    return length(center.xz - float2(e.worldX, e.worldZ)) <= max(e.radius + pageRadius, 1e-3);
}
float eventAtPosition(GPUWaterInteractionEvent e, float2 worldXZ) {
    float radius = max(e.radius, 1e-3);
    float x = saturate(1.0 - length(worldXZ - float2(e.worldX, e.worldZ)) / radius);
    return x * x * (3.0 - 2.0 * x) * e.strength;
}

GPUWaterStateCell sampleGrid(uint physical, float2 uv) {
    uv = saturate(uv);
    float2 p = uv * float(StateResolution - 1u);
    uint2 a = uint2(floor(p));
    uint2 b = min(a + 1u, uint2(StateResolution - 1u));
    float2 f = frac(p);
    uint base = physical * StateCellsPerPage;
    GPUWaterStateCell s00=stateCellsRead[base+a.y*StateResolution+a.x];
    GPUWaterStateCell s10=stateCellsRead[base+a.y*StateResolution+b.x];
    GPUWaterStateCell s01=stateCellsRead[base+b.y*StateResolution+a.x];
    GPUWaterStateCell s11=stateCellsRead[base+b.y*StateResolution+b.x];
    GPUWaterStateCell o;
    o.height=lerp(lerp(s00.height,s10.height,f.x),lerp(s01.height,s11.height,f.x),f.y);
    o.velocity=lerp(lerp(s00.velocity,s10.velocity,f.x),lerp(s01.velocity,s11.velocity,f.x),f.y);
    o.foam=lerp(lerp(s00.foam,s10.foam,f.x),lerp(s01.foam,s11.foam,f.x),f.y);
    o.reserved=0; return o;
}

bool sampleNeighbour(uint pageId, float3 worldPosition, out GPUWaterStateCell state) {
    state = (GPUWaterStateCell)0;
    if(pageId == InvalidPhysicalPage || pageId >= push.pageCount) return false;
    uint physical = pageTable[pageId];
    if(physical == InvalidPhysicalPage || physical >= MaxPhysicalStatePages || physicalOwners[physical] != pageId)
        return false;
    GPUVirtualWaterPage page = pages[pageId];
    RendererInstanceData instance = instances[page.instanceIndex];
    state = sampleGrid(physical, worldToPageUv(page, instance, worldPosition));
    return true;
}

bool sampleCurrentOrNeighbour(uint pageId, GPUVirtualWaterPage page, RendererInstanceData instance,
                              uint physical, float3 worldPosition, out GPUWaterStateCell state) {
    float2 uv = worldToPageUv(page, instance, worldPosition);
    if(all(uv >= 0.0) && all(uv <= 1.0)) { state = sampleGrid(physical, uv); return true; }
    uint neighbour = InvalidPhysicalPage;
    if(uv.x < 0.0) neighbour = uv.y < 0.5 ? page.neighborLeft0 : page.neighborLeft1;
    else if(uv.x > 1.0) neighbour = uv.y < 0.5 ? page.neighborRight0 : page.neighborRight1;
    else if(uv.y < 0.0) neighbour = uv.x < 0.5 ? page.neighborBottom0 : page.neighborBottom1;
    else if(uv.y > 1.0) neighbour = uv.x < 0.5 ? page.neighborTop0 : page.neighborTop1;
    return sampleNeighbour(neighbour, worldPosition, state);
}

float gerstnerJacobian(MaterialData material, float2 worldXZ, float time, float cellSize, uint waveMask) {
    float dxx = 1.0, dxz = 0.0, dzx = 0.0, dzz = 1.0;
    [unroll] for(uint i=0;i<8;++i) {
        if(i>=material.waterWaveCount.x) break;
        if((waveMask & (1u<<i)) == 0u) continue;
        float4 w = material.waterWaves[i];
        float lod = cellSize > 0.0 ? smoothstep(2.0, 6.0, w.w / cellSize) : 1.0;
        if(lod <= 0.0) continue;
        float2 dir = normalize(w.xy);
        float k = TwoPi / max(w.w, 1e-3);
        float phase = k * dot(dir, worldXZ) + material.waterWaveMotion[i].x * time;
        float qak = material.waterWaveMotion[i].y * w.z * k * sin(phase) * lod;
        dxx -= qak * dir.x * dir.x;
        dxz -= qak * dir.x * dir.y;
        dzx -= qak * dir.x * dir.y;
        dzz -= qak * dir.y * dir.y;
    }
    return dxx*dzz - dxz*dzx;
}

[numthreads(16,16,1)]
[shader("compute")]
void main(uint3 groupId : SV_GroupID, uint3 localId : SV_GroupThreadID) {
    const uint pageId = groupId.z;
    if (pageId >= push.pageCount) return;

    GPUVirtualWaterPage page = pages[pageId];
    RendererInstanceData instance = instances[page.instanceIndex];
    const uint quality = history[pageId].packedQuality;
    const bool visible = (quality & 0x80000000u) != 0;
    const uint foamTier = (quality >> 4u) & 3u;

    if (localId.x == 0u && localId.y == 0u) {
        bool eventHit = false;
        [loop] for (uint eventIndex = 0u; eventIndex < push.eventCount; ++eventIndex)
            eventHit = eventHit || eventTouchesPage(events[eventIndex], page, instance);

        const uint physical = pageTable[pageId];
        groupPhysical = (physical != InvalidPhysicalPage && physical < MaxPhysicalStatePages &&
                         physicalOwners[physical] == pageId) ? physical : InvalidPhysicalPage;
        groupNewAllocation = groupPhysical != InvalidPhysicalPage &&
                             physicalStates[groupPhysical].inactiveSeconds < 0.0 ? 1u : 0u;
        const bool stateOnly=(page.flags&1u)==0u;
        groupVisible = (visible||stateOnly) ? 1u : 0u;
        groupFoamTier = foamTier;
        groupEventHit = eventHit ? 1u : 0u;
        groupMaxFoamBits = 0u;
        groupMaxInteractionBits = 0u;
    }

    DeviceMemoryBarrierWithGroupSync();
    const uint physical = groupPhysical;
    if (physical == InvalidPhysicalPage || physicalOwners[physical] != pageId) return;

    const uint2 coord = localId.xy;
    const uint localIndex = coord.y * StateResolution + coord.x;
    const uint stateIndex = physical * StateCellsPerPage + localIndex;
    const float2 uv = float2(coord) / float(StateResolution - 1u);
    float3 localPosition = float3(lerp(page.minX, page.maxX, uv.x), page.baseHeight,
                                  lerp(page.minZ, page.maxZ, uv.y));
    float3 worldPosition = localToWorld(localPosition, instance);

    float2 authoredFlow = float2(0.0);
    if(page.flowSpeed > 1.0e-4) {
        float3 worldFlow3 = rotateQuaternion(float3(page.flowX,0.0,page.flowZ), instance.rotation);
        float2 worldFlow = worldFlow3.xz;
        float worldFlowLength = length(worldFlow);
        if(worldFlowLength > 1.0e-5) authoredFlow = worldFlow / worldFlowLength * page.flowSpeed;
    }

    GPUWaterStateCell previous = groupNewAllocation != 0u ? (GPUWaterStateCell)0 : stateCellsRead[stateIndex];
    if(groupNewAllocation == 0u && length(authoredFlow) > 1.0e-5) {
        GPUWaterStateCell advectedPrevious = (GPUWaterStateCell)0;
        float3 previousPosition = worldPosition;
        previousPosition.xz -= authoredFlow * max(push.deltaTime,0.0);
        if(sampleCurrentOrNeighbour(pageId,page,instance,physical,previousPosition,advectedPrevious))
            previous = advectedPrevious;
    }
    float leftH=previous.height,rightH=previous.height,bottomH=previous.height,topH=previous.height;
    float neighbourFoam = 0.0;
    GPUWaterStateCell neighbour;

    if(groupNewAllocation == 0u) {
        if(coord.x>0u) leftH=stateCellsRead[stateIndex-1u].height;
        else {
            uint n = coord.y < StateResolution/2u ? page.neighborLeft0 : page.neighborLeft1;
            if(sampleNeighbour(n,worldPosition,neighbour)){leftH=neighbour.height;neighbourFoam=max(neighbourFoam,neighbour.foam);}
        }
        if(coord.x+1u<StateResolution) rightH=stateCellsRead[stateIndex+1u].height;
        else {
            uint n = coord.y < StateResolution/2u ? page.neighborRight0 : page.neighborRight1;
            if(sampleNeighbour(n,worldPosition,neighbour)){rightH=neighbour.height;neighbourFoam=max(neighbourFoam,neighbour.foam);}
        }
        if(coord.y>0u) bottomH=stateCellsRead[stateIndex-StateResolution].height;
        else {
            uint n = coord.x < StateResolution/2u ? page.neighborBottom0 : page.neighborBottom1;
            if(sampleNeighbour(n,worldPosition,neighbour)){bottomH=neighbour.height;neighbourFoam=max(neighbourFoam,neighbour.foam);}
        }
        if(coord.y+1u<StateResolution) topH=stateCellsRead[stateIndex+StateResolution].height;
        else {
            uint n = coord.x < StateResolution/2u ? page.neighborTop0 : page.neighborTop1;
            if(sampleNeighbour(n,worldPosition,neighbour)){topH=neighbour.height;neighbourFoam=max(neighbourFoam,neighbour.foam);}
        }
    }

    float eventImpulse=0.0;
    [loop] for(uint eventIndex=0u;eventIndex<push.eventCount;++eventIndex)
        eventImpulse += eventAtPosition(events[eventIndex],worldPosition.xz);

    const float dt=max(push.deltaTime,0.0);
    GPUWaterStateCell state=previous;
    float laplacian=leftH+rightH+bottomH+topH-4.0*previous.height;
    // Stable explicit wave step. Cell spacing is included so coarse pages do
    // not propagate disturbances faster merely because their texels are larger.
    float horizontalScale=max(0.5*(abs(instance.scaleBase.x)+abs(instance.scaleBase.z)),1.0e-3);
    float worldCell=max(page.cellSize*8.0*horizontalScale/(StateResolution-1u),0.05);
    const float waveSpeed=4.25;
    const float cfl=waveSpeed*dt/worldCell;
    const float stableScale=min(1.0,0.45/max(cfl,1e-4));
    const float stepDt=dt*stableScale;
    state.velocity += (laplacian*(waveSpeed*waveSpeed)/(worldCell*worldCell)-previous.height*4.5)*stepDt;
    state.velocity += eventImpulse*3.0;
    state.velocity *= exp(-2.7*dt);
    state.height=clamp(previous.height+state.velocity*dt,-0.75,0.75);

    MaterialData material=materials[asuint(instance.positionMaterial.w)];
    const float jacobianCell=(page.flags&1u)==0u?0.0:page.cellSize;
    float jacobian=gerstnerJacobian(material,worldPosition.xz,frame.windGustFrequencyTime.z,
                                    jacobianCell,page.waveCandidateMask);
    float jacobianFoam=saturate((material.waterFoam.y-jacobian)*3.0);
    float unresolvedFoam=saturate(page.unresolvedSlopeVariance*5.0)*(float(groupFoamTier)/3.0);
    float crestInjection=max(jacobianFoam,unresolvedFoam);
    // Semi-Lagrangian foam advection in world space. Sampling may cross a
    // logical page boundary and therefore naturally exchanges state with the
    // linked same-LOD or 2:1 neighbour page.
    float2 flow=authoredFlow;
    if(length(flow)<=1e-5) {
        flow=frame.windDirectionStrength.xz;
        float flowLength=length(flow);
        flow=(flowLength>1e-5?flow/flowLength:float2(1,0))*(0.55+0.15*frame.windDirectionStrength.w);
    }
    float3 backPosition=worldPosition;backPosition.xz-=flow*dt;
    GPUWaterStateCell advectedState=(GPUWaterStateCell)0;
    float sourceFoam=previous.foam;
    if(groupNewAllocation==0u&&sampleCurrentOrNeighbour(pageId,page,instance,physical,backPosition,advectedState))
        sourceFoam=advectedState.foam;
    float advected=max(sourceFoam,neighbourFoam*0.94)*exp(-0.45*dt);
    state.foam=saturate(max(advected,crestInjection)+abs(eventImpulse)*0.42+abs(state.velocity)*0.006);
    state.reserved=0.0;

    if(physicalOwners[physical]==pageId) stateCellsWrite[stateIndex]=state;

    uint ignored=0u;
    InterlockedMax(groupMaxFoamBits,asuint(max(state.foam,0.0)),ignored);
    float interactionEnergy=abs(state.height)+abs(state.velocity)*0.12;
    InterlockedMax(groupMaxInteractionBits,asuint(max(interactionEnergy,0.0)),ignored);
    GroupMemoryBarrierWithGroupSync();

    if(localId.x==0u&&localId.y==0u&&physicalOwners[physical]==pageId){
        GPUWaterPhysicalState summary=physicalStates[physical];
        summary.foam=asfloat(groupMaxFoamBits);
        summary.interaction=asfloat(groupMaxInteractionBits);
        summary.interactionVelocity=0.0;
        summary.inactiveSeconds=(groupVisible!=0u||groupEventHit!=0u||groupNewAllocation!=0u)?0.0:max(summary.inactiveSeconds,0.0)+dt;
        physicalStates[physical]=summary;
        if(summary.inactiveSeconds>12.0&&summary.foam<0.01&&summary.interaction<0.005){
            uint observedOwner=InvalidPhysicalPage;
            InterlockedCompareExchange(physicalOwners[physical],pageId,InvalidPhysicalPage,observedOwner);
            if(observedOwner==pageId){uint observedMapping=InvalidPhysicalPage;
                InterlockedCompareExchange(pageTable[pageId],physical,InvalidPhysicalPage,observedMapping);}
        }
    }
}
