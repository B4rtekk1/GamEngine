import Common.SceneData;
import Water.VirtualWaterShared;

[[vk::binding(1,0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(2,0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(5,0)]] StructuredBuffer<RendererInstanceData> instances;
[[vk::binding(0,1)]] StructuredBuffer<GPUVirtualWaterPage> pages;
[[vk::binding(1,1)]] StructuredBuffer<GPUVisibleWaterPage> visiblePages;
[[vk::binding(2,1)]] StructuredBuffer<GPUWaterStatePhysicalRef> statePageTable;
[[vk::binding(3,1)]] StructuredBuffer<GPUWaterPhysicalState> physicalStates;
[[vk::binding(4,1)]] StructuredBuffer<GPUWaterStateCell> stateCells;

static const uint StitchLeft=1u, StitchRight=2u, StitchBottom=4u, StitchTop=8u;

float edgeEffectiveCell(float2 uv,float cell,uint mask){
    const float width=1.0/8.0;
    float a=0.0;
    if((mask&StitchLeft)!=0) a=max(a,1.0-saturate(uv.x/width));
    if((mask&StitchRight)!=0) a=max(a,1.0-saturate((1.0-uv.x)/width));
    if((mask&StitchBottom)!=0) a=max(a,1.0-saturate(uv.y/width));
    if((mask&StitchTop)!=0) a=max(a,1.0-saturate((1.0-uv.y)/width));
    return lerp(cell,cell*2.0,a);
}

void evaluateWavesCurrentPrevious(MaterialData m,float2 xz,float currentTime,float previousTime,float cell,uint mask,
                                  out float3 d,out float3 t,out float3 b,
                                  out float3 pd,out float3 pt,out float3 pb,
                                  out float unresolvedVariance){
 d=0;t=float3(1,0,0);b=float3(0,0,1);
 pd=0;pt=float3(1,0,0);pb=float3(0,0,1);unresolvedVariance=0.0;
 const float frameDt=max(currentTime-previousTime,0.0);
 [unroll] for(uint i=0;i<8;++i){
   if(i>=m.waterWaveCount.x) break;
   float4 w=m.waterWaves[i]; float lod=cell>0?smoothstep(2.0,6.0,w.w/cell):1.0;
   float k=WATER_TWO_PI/max(w.w,0.001);float slope=w.z*k;
   unresolvedVariance+=0.5*(1.0-lod*lod)*slope*slope;
   if((mask&(1u<<i))==0||lod<=0) continue;
   float2 dir=normalize(w.xy);
   float omega=m.waterWaveMotion[i].x;
   float phase=k*dot(dir,xz)+omega*currentTime; float s,c; sincos(phase,s,c);
   // Reconstruct the previous phase from the current sin/cos. This avoids a
   // second transcendental evaluation per wave/vertex and keeps motion vectors
   // exactly phase coherent for variable frame dt.
   float sd,cd; waterSinCosApprox(omega*frameDt,sd,cd);
   float ps=s*cd-c*sd;
   float pc=c*cd+s*sd;
   float h=m.waterWaveMotion[i].y*w.z;
   d+=lod*float3(h*dir.x*c,w.z*s,h*dir.y*c);
   t+=lod*float3(-h*dir.x*dir.x*k*s,w.z*dir.x*k*c,-h*dir.x*dir.y*k*s);
   b+=lod*float3(-h*dir.x*dir.y*k*s,w.z*dir.y*k*c,-h*dir.y*dir.y*k*s);
   pd+=lod*float3(h*dir.x*pc,w.z*ps,h*dir.y*pc);
   pt+=lod*float3(-h*dir.x*dir.x*k*ps,w.z*dir.x*k*pc,-h*dir.x*dir.y*k*ps);
   pb+=lod*float3(-h*dir.x*dir.y*k*ps,w.z*dir.y*k*pc,-h*dir.y*dir.y*k*ps);
 }
}

float2 octEncode(float3 n){
 n/=abs(n.x)+abs(n.y)+abs(n.z); float2 e=n.xz;
 if(n.y<0) e=(1.0-abs(e.yx))*float2(e.x>=0?1:-1,e.y>=0?1:-1);
 return e;
}

GPUWaterStateCell sampleStateGrid(GPUWaterStatePhysicalRef ref,float2 uv){
    GPUWaterStateCell zero=(GPUWaterStateCell)0;
    if(!waterStateRefValid(ref)) return zero;
    GPUWaterPhysicalState owner=physicalStates[ref.slot];
    if(owner.generation!=ref.generation) return zero;
    uv=saturate(uv);
    uint resolution=waterStateTierResolution(ref.tier);
    float2 p=uv*float(max(resolution,2u)-1u);
    uint2 a=uint2(floor(p));
    uint2 b=min(a+1u,uint2(resolution-1u));
    float2 f=frac(p);
    uint base=ref.slot*WATER_STATE_CELLS_PER_PAGE;
    GPUWaterStateCell s00=stateCells[base+a.y*WATER_STATE_RESOLUTION+a.x];
    GPUWaterStateCell s10=stateCells[base+a.y*WATER_STATE_RESOLUTION+b.x];
    GPUWaterStateCell s01=stateCells[base+b.y*WATER_STATE_RESOLUTION+a.x];
    GPUWaterStateCell s11=stateCells[base+b.y*WATER_STATE_RESOLUTION+b.x];
    GPUWaterStateCell o;
    o.height=lerp(lerp(s00.height,s10.height,f.x),lerp(s01.height,s11.height,f.x),f.y);
    o.velocityX=lerp(lerp(s00.velocityX,s10.velocityX,f.x),lerp(s01.velocityX,s11.velocityX,f.x),f.y);
    o.velocityZ=lerp(lerp(s00.velocityZ,s10.velocityZ,f.x),lerp(s01.velocityZ,s11.velocityZ,f.x),f.y);
    o.foam=lerp(lerp(s00.foam,s10.foam,f.x),lerp(s01.foam,s11.foam,f.x),f.y);
    return o;
}

struct VSIn {
 [[vk::location(0)]] float3 position:POSITION;
 [[vk::location(2)]] float2 uv:TEXCOORD0;
 [[vk::location(8)]] uint materialIndex:MATERIAL_INDEX;
};
struct VSOut {
 float4 position:SV_Position;
 [[vk::location(0)]] float3 worldPosition;
 [[vk::location(1)]] float3 normal;
 [[vk::location(2)]] nointerpolation uint materialIndex;
 [[vk::location(3)]] nointerpolation uint packedQuality;
 [[vk::location(4)]] float effectiveRoughness;
 [[vk::location(5)]] noperspective float2 currentNdc;
 [[vk::location(6)]] float4 previousClip;
 [[vk::location(7)]] nointerpolation uint pageIndex;
 [[vk::location(8)]] float2 localPageUv;
};

[shader("vertex")]
VSOut vertexMain(VSIn input,uint instanceId:SV_InstanceID,uint baseInstance:SV_StartInstanceLocation){
 GPUVisibleWaterPage v=visiblePages[baseInstance+instanceId]; GPUVirtualWaterPage p=pages[v.pageIndex];
 RendererInstanceData inst=instances[v.instanceIndex];
 float pageSize=waterPageSize(p); float2 pageOrigin=waterPageOrigin(p,inst,frame.cameraPosition.xyz);
 float2 localXZ;
 if((p.flags&WATER_PAGE_SPLINE)!=0u){
   float along=saturate(input.position.x);float across=input.position.z*2.0-1.0;
   float2 a=float2(p.splineStartX,p.splineStartZ),b=float2(p.splineEndX,p.splineEndZ);
   float2 d=b-a;float dl=max(length(d),1e-5);float2 side=float2(-d.y,d.x)/dl;
   float halfWidth=lerp(p.splineHalfWidth0,p.splineHalfWidth1,along);
   localXZ=lerp(a,b,along)+side*across*halfWidth;
 } else localXZ=pageOrigin+input.position.xz*pageSize;
 float localHeight=(p.flags&WATER_PAGE_SPLINE)!=0u?lerp(p.baseHeight,p.reservedFloat,saturate(input.position.x)):p.baseHeight;
 float3 localBase=float3(localXZ.x,localHeight,localXZ.y); float3 worldBase=waterLocalToWorld(localBase,inst);
 uint mi=asuint(inst.positionMaterial.w)+input.materialIndex; MaterialData m=materials[mi];
 uint packedQuality=v.packedQuality;
 if((p.executionFlags&WATER_EXEC_SIMPLE_SHADING)!=0u) packedQuality|=WATER_SIMPLE_SHADING_FLAG;
 GPUWaterStatePhysicalRef physical=statePageTable[v.pageIndex];
 GPUWaterStateCell localState=(GPUWaterStateCell)0;
 float2 interactionSlope=0.0;
 float interactionEnergy=0.0;
 if(waterStateRefValid(physical)&&physicalStates[physical.slot].generation==physical.generation){
   GPUWaterPhysicalState summary=physicalStates[physical.slot];
   float2 stateUv=waterStateUvFromWorld(p,inst,summary,frame.cameraPosition.xyz,worldBase);
   localState=sampleStateGrid(physical,stateUv);
   float stateDu=1.0/float(max(waterStateTierResolution(physical.tier),2u)-1u);
   float hL=sampleStateGrid(physical,stateUv-float2(stateDu,0)).height;
   float hR=sampleStateGrid(physical,stateUv+float2(stateDu,0)).height;
   float hB=sampleStateGrid(physical,stateUv-float2(0,stateDu)).height;
   float hT=sampleStateGrid(physical,stateUv+float2(0,stateDu)).height;
   float worldStep=max(waterStateWorldSize(p)*stateDu,1e-4);
   interactionSlope=float2((hR-hL),(hT-hB))/(2.0*worldStep);
   interactionEnergy=abs(localState.height)+length(float2(localState.velocityX,localState.velocityZ))*0.12;
   if(interactionEnergy>0.015||localState.foam>0.05||summary.foam>0.10) packedQuality|=WATER_REACTIVE_FLAG;
   worldBase+=waterRotateQuaternion(float3(0,localState.height,0),inst.rotation);
 }
 float cell=edgeEffectiveCell(input.position.xz,p.cellSize,p.spectralEdgeMask);
 float3 d,t,b,pd,pt,pb;float unresolvedVariance;
 evaluateWavesCurrentPrevious(m,worldBase.xz,frame.windGustFrequencyTime.z,frame.windGustFrequencyTime.w,
                              cell,v.waveCandidateMask,d,t,b,pd,pt,pb,unresolvedVariance);
 float3 world=worldBase+waterRotateQuaternion(d*inst.scaleBase.xyz,inst.rotation);
 float3 tw=normalize(waterRotateQuaternion(t,inst.rotation)); float3 bw=normalize(waterRotateQuaternion(b,inst.rotation));
 float3 n=normalize(cross(bw,tw));
 float3 interactionSlopeWorld=waterRotateQuaternion(float3(-interactionSlope.x,0,-interactionSlope.y),inst.rotation);
 n=normalize(n+interactionSlopeWorld);
 float4 clip=mul(mul(float4(world,1),frame.view),frame.projection);
 float3 previousBase=waterLocalToWorld(localBase,inst)+waterRotateQuaternion(float3(0,localState.height,0),inst.rotation);
 float3 prevWorld=previousBase+waterRotateQuaternion(pd*inst.scaleBase.xyz,inst.rotation);
 VSOut o; o.position=clip;o.worldPosition=world;o.normal=n;o.materialIndex=mi;o.packedQuality=packedQuality;
 float baseR=clamp(m.waterShallowColorRoughness.w,0.045,1.0);
 o.effectiveRoughness=clamp(sqrt(baseR*baseR+2.0*unresolvedVariance)+interactionEnergy*0.08,0.045,1.0);
 o.currentNdc=clip.xy/clip.w; o.previousClip=mul(mul(float4(prevWorld,1),frame.previousView),frame.previousProjection);
 o.pageIndex=v.pageIndex;o.localPageUv=input.position.xz;return o;
}

struct PSOut {
 [[vk::location(0)]] float4 surface:SV_Target0;
 [[vk::location(1)]] float2 meta:SV_Target1;
 [[vk::location(2)]] float2 velocity:SV_Target2;
};
[shader("fragment")]
PSOut fragmentMain(VSOut i){
 GPUVirtualWaterPage p=pages[i.pageIndex];
 if(p.coverageClass==0u) discard;
 if(p.coverageClass==2u){
   uint2 c=min(uint2(saturate(i.localPageUv)*8.0),uint2(7u));uint bit=c.y*8u+c.x;
   uint word=bit<32u?p.cellCoverageMaskLow:p.cellCoverageMaskHigh;
   if((word&(1u<<(bit&31u)))==0u) discard;
 }
 PSOut o; o.surface=float4(octEncode(normalize(i.normal)),i.position.z,i.effectiveRoughness);
 o.meta=float2(asfloat(i.materialIndex),asfloat(i.packedQuality));
 float2 prev=i.previousClip.xy/max(abs(i.previousClip.w),1e-5); o.velocity=(i.currentNdc-prev)*0.5; return o;
}
