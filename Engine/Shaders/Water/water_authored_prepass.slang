import Common.SceneData;
import Water.VirtualWaterShared;

[[vk::binding(1,0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(2,0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(5,0)]] StructuredBuffer<RendererInstanceData> visibleInstances;
[[vk::binding(6,0)]] StructuredBuffer<uint> visibleInstanceIndices;
[[vk::binding(8,0)]] StructuredBuffer<RendererPreviousTransformData> previousTransforms;

struct GPUAuthoredWaterBody { uint instanceIndex,statePageIndex,bodyType,executionFlags; };
struct GPUAuthoredWaterConfig { uint bodyCount,padding0,padding1,padding2; GPUAuthoredWaterBody bodies[16]; };
[[vk::binding(0,1)]] ConstantBuffer<GPUAuthoredWaterConfig> authoredConfig;
[[vk::binding(1,1)]] StructuredBuffer<GPUVirtualWaterPage> pages;
[[vk::binding(2,1)]] StructuredBuffer<GPUWaterStatePhysicalRef> statePageTable;
[[vk::binding(3,1)]] StructuredBuffer<GPUWaterPhysicalState> physicalStates;
[[vk::binding(4,1)]] StructuredBuffer<GPUWaterStateCell> stateCells;

float2 octEncode(float3 n){n/=abs(n.x)+abs(n.y)+abs(n.z);float2 e=n.xz;if(n.y<0)e=(1.0-abs(e.yx))*float2(e.x>=0?1:-1,e.y>=0?1:-1);return e;}

GPUWaterStateCell sampleState(GPUWaterStatePhysicalRef ref,float2 uv){
 GPUWaterStateCell zero=(GPUWaterStateCell)0;if(!waterStateRefValid(ref))return zero;GPUWaterPhysicalState owner=physicalStates[ref.slot];if(owner.generation!=ref.generation)return zero;
 uint res=waterStateTierResolution(ref.tier);uv=saturate(uv);float2 p=uv*float(max(res,2u)-1u);uint2 a=uint2(floor(p));uint2 b=min(a+1u,uint2(res-1u));float2 f=frac(p);uint base=ref.slot*WATER_STATE_CELLS_PER_PAGE;
 GPUWaterStateCell s00=stateCells[base+a.y*WATER_STATE_RESOLUTION+a.x],s10=stateCells[base+a.y*WATER_STATE_RESOLUTION+b.x],s01=stateCells[base+b.y*WATER_STATE_RESOLUTION+a.x],s11=stateCells[base+b.y*WATER_STATE_RESOLUTION+b.x];GPUWaterStateCell o;
 o.height=lerp(lerp(s00.height,s10.height,f.x),lerp(s01.height,s11.height,f.x),f.y);o.velocityX=lerp(lerp(s00.velocityX,s10.velocityX,f.x),lerp(s01.velocityX,s11.velocityX,f.x),f.y);o.velocityZ=lerp(lerp(s00.velocityZ,s10.velocityZ,f.x),lerp(s01.velocityZ,s11.velocityZ,f.x),f.y);o.foam=lerp(lerp(s00.foam,s10.foam,f.x),lerp(s01.foam,s11.foam,f.x),f.y);return o;
}

void evaluateCurrentPrevious(MaterialData m,float2 currentXZ,float2 previousXZ,float currentTime,float previousTime,float cell,
                             out float3 d,out float3 tangent,out float3 binormal,out float3 pd){
 d=0;pd=0;tangent=float3(1,0,0);binormal=float3(0,0,1);
 [unroll]for(uint index=0;index<8;++index){if(index>=m.waterWaveCount.x)break;float4 wave=m.waterWaves[index];float lod=cell>0?smoothstep(2.0,6.0,wave.w/cell):1.0;if(lod<=0)continue;
   float2 dir=normalize(wave.xy);float k=WATER_TWO_PI/max(wave.w,0.001);float omega=m.waterWaveMotion[index].x;
   float phase=k*dot(dir,currentXZ)+omega*currentTime;float s,c;sincos(phase,s,c);
   float delta=k*dot(dir,currentXZ-previousXZ)+omega*(currentTime-previousTime);float sd,cd;waterSinCosApprox(delta,sd,cd);float ps=s*cd-c*sd;float pc=c*cd+s*sd;
   float horizontal=m.waterWaveMotion[index].y*wave.z;d+=lod*float3(horizontal*dir.x*c,wave.z*s,horizontal*dir.y*c);pd+=lod*float3(horizontal*dir.x*pc,wave.z*ps,horizontal*dir.y*pc);
   tangent+=lod*float3(-horizontal*dir.x*dir.x*k*s,wave.z*dir.x*k*c,-horizontal*dir.x*dir.y*k*s);binormal+=lod*float3(-horizontal*dir.x*dir.y*k*s,wave.z*dir.y*k*c,-horizontal*dir.y*dir.y*k*s);}
}

struct VSIn{[[vk::location(0)]] float3 position:POSITION;[[vk::location(2)]] float2 uv:TEXCOORD0;[[vk::location(4)]] float2 uv1:TEXCOORD1;[[vk::location(8)]] uint materialIndex:MATERIAL_INDEX;};
struct VSOut{float4 position:SV_Position;[[vk::location(0)]] float3 worldPosition;[[vk::location(1)]] float3 normal;[[vk::location(2)]] nointerpolation uint materialIndex;[[vk::location(3)]] nointerpolation uint packedQuality;[[vk::location(4)]] float effectiveRoughness;[[vk::location(5)]] noperspective float2 currentNdc;[[vk::location(6)]] float4 previousClip;};

[shader("vertex")]
VSOut vertexMain(VSIn input,uint instanceIndex:SV_InstanceID,uint baseInstance:SV_StartInstanceLocation){
 const uint visibleIndex=visibleInstanceIndices[baseInstance+instanceIndex];RendererInstanceData instance=visibleInstances[visibleIndex];
 uint statePage=WATER_FAR_ANALYTIC_PAGE_ID;uint executionFlags=1u;
 [loop]for(uint body=0;body<authoredConfig.bodyCount;++body){if(authoredConfig.bodies[body].instanceIndex==visibleIndex){statePage=authoredConfig.bodies[body].statePageIndex;executionFlags=authoredConfig.bodies[body].executionFlags;break;}}
 GPUWaterStateCell localState=(GPUWaterStateCell)0;float2 interactionSlope=0;float interactionEnergy=0;
 if(statePage!=WATER_FAR_ANALYTIC_PAGE_ID&&statePage<0x1fffu){GPUWaterStatePhysicalRef ref=statePageTable[statePage];if(waterStateRefValid(ref)&&physicalStates[ref.slot].generation==ref.generation){GPUVirtualWaterPage page=pages[statePage];GPUWaterPhysicalState summary=physicalStates[ref.slot];float3 authoredWorld=waterLocalToWorld(input.position,instance);float2 suv=waterStateUvFromWorld(page,instance,summary,frame.cameraPosition.xyz,authoredWorld);localState=sampleState(ref,suv);float du=1.0/float(max(waterStateTierResolution(ref.tier),2u)-1u);float hL=sampleState(ref,suv-float2(du,0)).height,hR=sampleState(ref,suv+float2(du,0)).height,hB=sampleState(ref,suv-float2(0,du)).height,hT=sampleState(ref,suv+float2(0,du)).height;float worldStep=max(waterStateWorldSize(page)*du,1e-4);interactionSlope=float2((hR-hL),(hT-hB))/max(2.0*worldStep,1e-4);interactionEnergy=abs(localState.height)+length(float2(localState.velocityX,localState.velocityZ))*0.12+max(localState.foam,summary.foam)*0.08;}}
 float3 baseWorld=waterLocalToWorld(input.position,instance)+waterRotateQuaternion(float3(0,localState.height,0),instance.rotation);uint materialIndex=asuint(instance.positionMaterial.w)+input.materialIndex;MaterialData material=materials[materialIndex];
 RendererPreviousTransformData previous=previousTransforms[visibleIndex];float3 previousBase=waterRotateQuaternion(input.position*previous.previousScale.xyz,previous.previousRotation)+previous.previousPosition.xyz+waterRotateQuaternion(float3(0,localState.height,0),previous.previousRotation);
 float3 displacement,tangent,binormal,pd;evaluateCurrentPrevious(material,baseWorld.xz,previousBase.xz,frame.windGustFrequencyTime.z,frame.windGustFrequencyTime.w,input.uv1.x,displacement,tangent,binormal,pd);
 float3 world=baseWorld+waterRotateQuaternion(displacement*instance.scaleBase.xyz,instance.rotation);float3 normal=normalize(cross(normalize(waterRotateQuaternion(binormal,instance.rotation)),normalize(waterRotateQuaternion(tangent,instance.rotation))));normal=normalize(normal+waterRotateQuaternion(float3(-interactionSlope.x,0,-interactionSlope.y),instance.rotation));float4 clip=mul(mul(float4(world,1),frame.view),frame.projection);
 float3 previousWorld=previousBase+waterRotateQuaternion(pd*previous.previousScale.xyz,previous.previousRotation);
 bool budgeted=(executionFlags&1u)!=0u;float distanceToCamera=length(frame.cameraPosition.xyz-world);uint tier=budgeted?(distanceToCamera<80?3u:(distanceToCamera<250?2u:1u)):1u;uint foam=tier>=2u?2u:1u;uint reactive=interactionEnergy>0.01?WATER_REACTIVE_FLAG:0u;uint packed=tier|(tier<<2u)|(foam<<4u)|((statePage&0x1fffu)<<8u)|((executionFlags&WATER_EXEC_SIMPLE_SHADING)!=0u?WATER_SIMPLE_SHADING_FLAG:0u)|WATER_AUTHORED_FLAG|reactive|WATER_VALID_FLAG;
 float baseR=clamp(material.waterShallowColorRoughness.w,0.045,1.0);VSOut o;o.position=clip;o.worldPosition=world;o.normal=normal;o.materialIndex=materialIndex;o.packedQuality=packed;o.effectiveRoughness=clamp(baseR+interactionEnergy*0.08,0.045,1.0);o.currentNdc=clip.xy/clip.w;o.previousClip=mul(mul(float4(previousWorld,1),frame.previousView),frame.previousProjection);return o;
}

struct PSOut{[[vk::location(0)]] float4 surface:SV_Target0;[[vk::location(1)]] float2 meta:SV_Target1;[[vk::location(2)]] float2 velocity:SV_Target2;};
[shader("fragment")]
PSOut fragmentMain(VSOut input,bool frontFace:SV_IsFrontFace){float3 n=normalize(input.normal);MaterialData m=materials[input.materialIndex];if((m.textureIndices.w&1)!=0&&!frontFace)n=-n;PSOut o;o.surface=float4(octEncode(n),input.position.z,input.effectiveRoughness);o.meta=float2(asfloat(input.materialIndex),asfloat(input.packedQuality));o.velocity=(input.currentNdc-input.previousClip.xy/max(abs(input.previousClip.w),1e-5))*0.5;return o;}
