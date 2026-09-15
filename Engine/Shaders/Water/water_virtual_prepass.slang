import Common.SceneData;

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

[[vk::binding(1,0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(2,0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(5,0)]] StructuredBuffer<RendererInstanceData> instances;
[[vk::binding(0,1)]] StructuredBuffer<GPUVirtualWaterPage> pages;
[[vk::binding(1,1)]] StructuredBuffer<GPUVisibleWaterPage> visiblePages;
[[vk::binding(2,1)]] StructuredBuffer<uint> statePageTable;
struct GPUWaterPhysicalState { float foam, interaction, interactionVelocity, inactiveSeconds; };
struct GPUWaterStateCell { float height, velocity, foam, reserved; };
[[vk::binding(3,1)]] StructuredBuffer<GPUWaterPhysicalState> physicalStates;
[[vk::binding(4,1)]] StructuredBuffer<GPUWaterStateCell> stateCells;
static const uint InvalidPhysicalPage=0xffffffffu;
static const uint StateResolution=16u;
static const uint StateCellsPerPage=StateResolution*StateResolution;

static const uint StitchLeft=1u, StitchRight=2u, StitchBottom=4u, StitchTop=8u;
static const float PI2=6.28318530718;

float3 rotateQuaternion(float3 p,float4 q){ return p+2.0*cross(q.xyz,cross(q.xyz,p)+q.w*p); }
float3 localToWorld(float3 p,RendererInstanceData i){ return rotateQuaternion(p*i.scaleBase.xyz,i.rotation)+i.positionMaterial.xyz; }

float edgeEffectiveCell(float2 uv,float cell,uint mask){
    const float width=1.0/8.0;
    float a=0.0;
    if((mask&StitchLeft)!=0) a=max(a,1.0-saturate(uv.x/width));
    if((mask&StitchRight)!=0) a=max(a,1.0-saturate((1.0-uv.x)/width));
    if((mask&StitchBottom)!=0) a=max(a,1.0-saturate(uv.y/width));
    if((mask&StitchTop)!=0) a=max(a,1.0-saturate((1.0-uv.y)/width));
    return lerp(cell,cell*2.0,a);
}

void evaluateWaves(MaterialData m,float2 xz,float time,float cell,uint mask,
                   out float3 d,out float3 t,out float3 b,out float unresolvedVariance){
 d=0;t=float3(1,0,0);b=float3(0,0,1);unresolvedVariance=0.0;
 [unroll] for(uint i=0;i<8;++i){
   if(i>=m.waterWaveCount.x) break;
   float4 w=m.waterWaves[i]; float lod=cell>0?smoothstep(2.0,6.0,w.w/cell):1.0;
   float k=PI2/max(w.w,0.001);float slope=w.z*k;
   unresolvedVariance+=0.5*(1.0-lod*lod)*slope*slope;
   if((mask&(1u<<i))==0||lod<=0) continue;
   float2 dir=normalize(w.xy);
   float phase=k*dot(dir,xz)+m.waterWaveMotion[i].x*time; float s,c; sincos(phase,s,c);
   float h=m.waterWaveMotion[i].y*w.z;
   d+=lod*float3(h*dir.x*c,w.z*s,h*dir.y*c);
   t+=lod*float3(-h*dir.x*dir.x*k*s,w.z*dir.x*k*c,-h*dir.x*dir.y*k*s);
   b+=lod*float3(-h*dir.x*dir.y*k*s,w.z*dir.y*k*c,-h*dir.y*dir.y*k*s);
 }
}
float2 octEncode(float3 n){
 n/=abs(n.x)+abs(n.y)+abs(n.z); float2 e=n.xz;
 if(n.y<0) e=(1.0-abs(e.yx))*float2(e.x>=0?1:-1,e.y>=0?1:-1);
 return e;
}

GPUWaterStateCell sampleStateGrid(uint physical,float2 uv){
    uv=saturate(uv);
    float2 p=uv*float(StateResolution-1u);
    uint2 a=uint2(floor(p));
    uint2 b=min(a+1u,uint2(StateResolution-1u));
    float2 f=frac(p);
    uint base=physical*StateCellsPerPage;
    GPUWaterStateCell s00=stateCells[base+a.y*StateResolution+a.x];
    GPUWaterStateCell s10=stateCells[base+a.y*StateResolution+b.x];
    GPUWaterStateCell s01=stateCells[base+b.y*StateResolution+a.x];
    GPUWaterStateCell s11=stateCells[base+b.y*StateResolution+b.x];
    GPUWaterStateCell x0,x1,o;
    x0.height=lerp(s00.height,s10.height,f.x);x0.velocity=lerp(s00.velocity,s10.velocity,f.x);x0.foam=lerp(s00.foam,s10.foam,f.x);x0.reserved=0;
    x1.height=lerp(s01.height,s11.height,f.x);x1.velocity=lerp(s01.velocity,s11.velocity,f.x);x1.foam=lerp(s01.foam,s11.foam,f.x);x1.reserved=0;
    o.height=lerp(x0.height,x1.height,f.y);o.velocity=lerp(x0.velocity,x1.velocity,f.y);o.foam=lerp(x0.foam,x1.foam,f.y);o.reserved=0;
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
};

[shader("vertex")]
VSOut vertexMain(VSIn input,uint instanceId:SV_InstanceID,uint baseInstance:SV_StartInstanceLocation){
 GPUVisibleWaterPage v=visiblePages[baseInstance+instanceId]; GPUVirtualWaterPage p=pages[v.pageIndex];
 RendererInstanceData inst=instances[v.instanceIndex];
 float pageSize=p.cellSize*8.0; float2 localXZ=float2(p.originX,p.originZ)+input.position.xz*pageSize;
 float3 localBase=float3(localXZ.x,p.baseHeight,localXZ.y); float3 worldBase=localToWorld(localBase,inst);
 uint mi=asuint(inst.positionMaterial.w)+input.materialIndex; MaterialData m=materials[mi];
 uint packedQuality=v.packedQuality;
 uint physical=statePageTable[v.pageIndex];
 GPUWaterStateCell localState=(GPUWaterStateCell)0;
 float2 interactionSlope=0.0;
 float interactionEnergy=0.0;
 if(physical!=InvalidPhysicalPage){
   localState=sampleStateGrid(physical,input.position.xz);
   float stateDu=1.0/float(StateResolution-1u);
   float hL=sampleStateGrid(physical,input.position.xz-float2(stateDu,0)).height;
   float hR=sampleStateGrid(physical,input.position.xz+float2(stateDu,0)).height;
   float hB=sampleStateGrid(physical,input.position.xz-float2(0,stateDu)).height;
   float hT=sampleStateGrid(physical,input.position.xz+float2(0,stateDu)).height;
   float worldStep=max(pageSize*stateDu,1e-4);
   interactionSlope=float2((hR-hL),(hT-hB))/(2.0*worldStep);
   GPUWaterPhysicalState summary=physicalStates[physical];
   interactionEnergy=abs(localState.height)+abs(localState.velocity)*0.12;
   if(interactionEnergy>0.015||localState.foam>0.05||summary.foam>0.10) packedQuality|=0x40000000u;
   // State-grid height is stored in world metres, so do not scale it again.
   worldBase+=rotateQuaternion(float3(0,localState.height,0),inst.rotation);
 }
 float cell=edgeEffectiveCell(input.position.xz,p.cellSize,p.spectralEdgeMask);
 float3 d,t,b;float unresolvedVariance; evaluateWaves(m,worldBase.xz,frame.windGustFrequencyTime.z,cell,v.waveCandidateMask,d,t,b,unresolvedVariance);
 float3 world=worldBase+rotateQuaternion(d*inst.scaleBase.xyz,inst.rotation);
 float3 tw=normalize(rotateQuaternion(t,inst.rotation)); float3 bw=normalize(rotateQuaternion(b,inst.rotation));
 float3 n=normalize(cross(bw,tw));
 float3 interactionSlopeWorld=rotateQuaternion(float3(-interactionSlope.x,0,-interactionSlope.y),inst.rotation);
 n=normalize(n+interactionSlopeWorld);
 float4 clip=mul(mul(float4(world,1),frame.view),frame.projection);
 // Sampling pages follow the camera; previous physical water position uses the current world anchor.
 float waveDt=max(frame.windGustFrequencyTime.z-frame.windGustFrequencyTime.w,0.0);
 float previousInteractionHeight=localState.height-localState.velocity*waveDt;
 float3 previousBase=localToWorld(localBase,inst)+rotateQuaternion(float3(0,previousInteractionHeight,0),inst.rotation);
 float3 pd,pt,pb;float previousVariance; evaluateWaves(m,previousBase.xz,frame.windGustFrequencyTime.w,cell,v.waveCandidateMask,pd,pt,pb,previousVariance);
 float3 prevWorld=previousBase+rotateQuaternion(pd*inst.scaleBase.xyz,inst.rotation);
 VSOut o; o.position=clip;o.worldPosition=world;o.normal=n;o.materialIndex=mi;
 o.packedQuality=packedQuality;
 float baseR=clamp(m.waterShallowColorRoughness.w,0.045,1.0);
 o.effectiveRoughness=clamp(sqrt(baseR*baseR+2.0*unresolvedVariance)+interactionEnergy*0.08,0.045,1.0);
 o.currentNdc=clip.xy/clip.w; o.previousClip=mul(mul(float4(prevWorld,1),frame.previousView),frame.previousProjection); return o;
}

struct PSOut {
 [[vk::location(0)]] float4 surface:SV_Target0;
 [[vk::location(1)]] float2 meta:SV_Target1;
 [[vk::location(2)]] float2 velocity:SV_Target2;
};
[shader("fragment")]
PSOut fragmentMain(VSOut i){
 PSOut o; o.surface=float4(octEncode(normalize(i.normal)),i.position.z,i.effectiveRoughness);
 o.meta=float2(asfloat(i.materialIndex),asfloat(i.packedQuality));
 float2 prev=i.previousClip.xy/i.previousClip.w; o.velocity=(i.currentNdc-prev)*0.5; return o;
}
