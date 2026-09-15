import Common.SceneData;

[[vk::binding(1,0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(2,0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(5,0)]] StructuredBuffer<RendererInstanceData> visibleInstances;
[[vk::binding(6,0)]] StructuredBuffer<uint> visibleInstanceIndices;
[[vk::binding(8,0)]] StructuredBuffer<RendererPreviousTransformData> previousTransforms;

struct GPUAuthoredWaterBody { uint instanceIndex,statePageIndex,bodyType,padding; };
struct GPUAuthoredWaterConfig { uint bodyCount,padding0,padding1,padding2; GPUAuthoredWaterBody bodies[16]; };
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
struct GPUWaterPhysicalState { float foam, interaction, interactionVelocity, inactiveSeconds; };
struct GPUWaterStateCell { float height, velocity, foam, reserved; };

[[vk::binding(0,1)]] ConstantBuffer<GPUAuthoredWaterConfig> authoredConfig;
[[vk::binding(1,1)]] StructuredBuffer<GPUVirtualWaterPage> pages;
[[vk::binding(2,1)]] StructuredBuffer<uint> statePageTable;
[[vk::binding(3,1)]] StructuredBuffer<GPUWaterPhysicalState> physicalStates;
[[vk::binding(4,1)]] StructuredBuffer<GPUWaterStateCell> stateCells;

static const float TwoPi=6.28318530718;
static const uint AuthoredWaterFlag=0x10000000u;
static const uint InvalidPhysicalPage=0xffffffffu;
static const uint InvalidStatePage=0x1fffu;
static const uint StateResolution=16u;
static const uint StateCellsPerPage=StateResolution*StateResolution;

float3 rotateQuaternion(float3 p,float4 q){return p+2.0*cross(q.xyz,cross(q.xyz,p)+q.w*p);}
float2 octEncode(float3 n){n/=abs(n.x)+abs(n.y)+abs(n.z);float2 e=n.xz;if(n.y<0)e=(1.0-abs(e.yx))*float2(e.x>=0?1:-1,e.y>=0?1:-1);return e;}

void evaluateWaves(MaterialData m,float2 worldXZ,float time,float cell,out float3 d,out float3 tangent,out float3 binormal){
 d=0;tangent=float3(1,0,0);binormal=float3(0,0,1);
 [unroll]for(uint index=0;index<8;++index){
   if(index>=m.waterWaveCount.x)break;float4 wave=m.waterWaves[index];
   float lod=cell>0?smoothstep(2.0,6.0,wave.w/cell):1.0;if(lod<=0)continue;
   float2 direction=normalize(wave.xy);float k=TwoPi/max(wave.w,0.001);
   float s,c;sincos(k*dot(direction,worldXZ)+m.waterWaveMotion[index].x*time,s,c);
   float horizontal=m.waterWaveMotion[index].y*wave.z;
   d+=lod*float3(horizontal*direction.x*c,wave.z*s,horizontal*direction.y*c);
   tangent+=lod*float3(-horizontal*direction.x*direction.x*k*s,wave.z*direction.x*k*c,-horizontal*direction.x*direction.y*k*s);
   binormal+=lod*float3(-horizontal*direction.x*direction.y*k*s,wave.z*direction.y*k*c,-horizontal*direction.y*direction.y*k*s);
 }
}

GPUWaterStateCell sampleState(uint physical,float2 uv){
 uv=saturate(uv);float2 p=uv*float(StateResolution-1u);uint2 a=uint2(floor(p));uint2 b=min(a+1u,uint2(StateResolution-1u));float2 f=frac(p);
 uint base=physical*StateCellsPerPage;GPUWaterStateCell s00=stateCells[base+a.y*StateResolution+a.x],s10=stateCells[base+a.y*StateResolution+b.x],s01=stateCells[base+b.y*StateResolution+a.x],s11=stateCells[base+b.y*StateResolution+b.x];GPUWaterStateCell o;
 o.height=lerp(lerp(s00.height,s10.height,f.x),lerp(s01.height,s11.height,f.x),f.y);o.velocity=lerp(lerp(s00.velocity,s10.velocity,f.x),lerp(s01.velocity,s11.velocity,f.x),f.y);o.foam=lerp(lerp(s00.foam,s10.foam,f.x),lerp(s01.foam,s11.foam,f.x),f.y);o.reserved=0;return o;
}

struct VSIn{
 [[vk::location(0)]] float3 position:POSITION;
 [[vk::location(2)]] float2 uv:TEXCOORD0;
 [[vk::location(4)]] float2 uv1:TEXCOORD1;
 [[vk::location(8)]] uint materialIndex:MATERIAL_INDEX;
};
struct VSOut{
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
VSOut vertexMain(VSIn input,uint instanceIndex:SV_InstanceID,uint baseInstance:SV_StartInstanceLocation){
 const uint visibleIndex=visibleInstanceIndices[baseInstance+instanceIndex];
 RendererInstanceData instance=visibleInstances[visibleIndex];
 uint statePage=InvalidStatePage;
 [loop]for(uint body=0;body<authoredConfig.bodyCount;++body){if(authoredConfig.bodies[body].instanceIndex==visibleIndex){statePage=authoredConfig.bodies[body].statePageIndex;break;}}

 GPUWaterStateCell localState=(GPUWaterStateCell)0;float2 interactionSlope=0.0;float interactionEnergy=0.0;
 if(statePage!=InvalidStatePage){
   uint physical=statePageTable[statePage];
   if(physical!=InvalidPhysicalPage){
     GPUVirtualWaterPage page=pages[statePage];
     float2 span=max(float2(page.maxX-page.minX,page.maxZ-page.minZ),float2(1e-4));
     float2 suv=(input.position.xz-float2(page.minX,page.minZ))/span;
     localState=sampleState(physical,suv);
     float du=1.0/float(StateResolution-1u);
     float hL=sampleState(physical,suv-float2(du,0)).height,hR=sampleState(physical,suv+float2(du,0)).height;
     float hB=sampleState(physical,suv-float2(0,du)).height,hT=sampleState(physical,suv+float2(0,du)).height;
     interactionSlope=float2((hR-hL)/max(2.0*span.x*du,1e-4),(hT-hB)/max(2.0*span.y*du,1e-4));
     GPUWaterPhysicalState summary=physicalStates[physical];interactionEnergy=abs(localState.height)+abs(localState.velocity)*0.12+max(localState.foam,summary.foam)*0.08;
   }
 }

 float3 baseWorld=rotateQuaternion(input.position*instance.scaleBase.xyz,instance.rotation)+instance.positionMaterial.xyz;
 baseWorld+=rotateQuaternion(float3(0,localState.height,0),instance.rotation);
 uint materialIndex=asuint(instance.positionMaterial.w)+input.materialIndex;MaterialData material=materials[materialIndex];
 float3 displacement,tangent,binormal;evaluateWaves(material,baseWorld.xz,frame.windGustFrequencyTime.z,input.uv1.x,displacement,tangent,binormal);
 float3 world=baseWorld+rotateQuaternion(displacement*instance.scaleBase.xyz,instance.rotation);
 float3 normal=normalize(cross(normalize(rotateQuaternion(binormal,instance.rotation)),normalize(rotateQuaternion(tangent,instance.rotation))));
 normal=normalize(normal+rotateQuaternion(float3(-interactionSlope.x,0,-interactionSlope.y),instance.rotation));
 float4 clip=mul(mul(float4(world,1),frame.view),frame.projection);

 RendererPreviousTransformData previous=previousTransforms[visibleIndex];
 float dt=max(frame.windGustFrequencyTime.z-frame.windGustFrequencyTime.w,0.0);
 float previousInteractionHeight=localState.height-localState.velocity*dt;
 float3 previousBase=rotateQuaternion(input.position*previous.previousScale.xyz,previous.previousRotation)+previous.previousPosition.xyz;
 previousBase+=rotateQuaternion(float3(0,previousInteractionHeight,0),previous.previousRotation);
 float3 pd,pt,pb;evaluateWaves(material,previousBase.xz,frame.windGustFrequencyTime.w,input.uv1.x,pd,pt,pb);
 float3 previousWorld=previousBase+rotateQuaternion(pd*previous.previousScale.xyz,previous.previousRotation);

 float distanceToCamera=length(frame.cameraPosition.xyz-world);uint tier=distanceToCamera<80.0?3u:(distanceToCamera<250.0?2u:1u);uint foam=tier>=2u?2u:1u;
 uint reactive=interactionEnergy>0.01?0x40000000u:0u;
 uint packed=tier|(tier<<2u)|(foam<<4u)|((statePage&0x1fffu)<<8u)|AuthoredWaterFlag|reactive|0x80000000u;
 float baseR=clamp(material.waterShallowColorRoughness.w,0.045,1.0);
 VSOut output;output.position=clip;output.worldPosition=world;output.normal=normal;output.materialIndex=materialIndex;output.packedQuality=packed;output.effectiveRoughness=clamp(baseR+interactionEnergy*0.08,0.045,1.0);output.currentNdc=clip.xy/clip.w;output.previousClip=mul(mul(float4(previousWorld,1),frame.previousView),frame.previousProjection);return output;
}

struct PSOut{[[vk::location(0)]] float4 surface:SV_Target0;[[vk::location(1)]] float2 meta:SV_Target1;[[vk::location(2)]] float2 velocity:SV_Target2;};
[shader("fragment")]
PSOut fragmentMain(VSOut input,bool frontFace:SV_IsFrontFace){
 float3 n=normalize(input.normal);MaterialData m=materials[input.materialIndex];if((m.textureIndices.w&1)!=0&&!frontFace)n=-n;
 PSOut output;output.surface=float4(octEncode(n),input.position.z,input.effectiveRoughness);output.meta=float2(asfloat(input.materialIndex),asfloat(input.packedQuality));output.velocity=(input.currentNdc-input.previousClip.xy/input.previousClip.w)*0.5;return output;
}
