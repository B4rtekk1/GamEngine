import Common.PbrLighting;
import Common.SceneData;
import Common.ReflectionFallback;

[[vk::binding(1,0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(2,0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(3,0)]] StructuredBuffer<ClusterLightRange> clusterRanges;
[[vk::binding(5,0)]] StructuredBuffer<RendererInstanceData> instances;
[[vk::binding(7,0)]] StructuredBuffer<uint> clusterLightIndices;
[[vk::binding(11,0)]] SamplerCube prefilteredEnvironmentMap;
[[vk::binding(13,0)]] Sampler2D materialTextures[];

struct ReflectionProbeData {
    float4 positionAndBlendDistance;
    float4 extentsAndShape;
    uint textureIndex; int priority; uint flags; uint padding;
};
[[vk::binding(14,0)]] StructuredBuffer<ReflectionProbeData> reflectionProbes;
[[vk::binding(15,0)]] SamplerCube reflectionProbeTextures[256];
static const uint Probe_Box=1u<<0u, Probe_BoxProject=1u<<1u, Probe_Enabled=1u<<2u;

[[vk::binding(0,1)]] Sampler2D waterSurface;
[[vk::binding(1,1)]] Sampler2D waterMeta;
[[vk::binding(2,1)]] Sampler2D opaqueColor;
[[vk::binding(3,1)]] Sampler2D opaqueDepth;
[[vk::binding(4,1)]] StructuredBuffer<uint> tileLists;
[[vk::binding(5,1)]] RWTexture2D<float4> waterLighting;
struct Settings { uint width,height,tilesX,maxTiles; };
[[vk::binding(6,1)]] ConstantBuffer<Settings> settings;
[[vk::binding(7,1)]] StructuredBuffer<uint> statePageTable;
struct GPUWaterPhysicalState { float foam, interaction, interactionVelocity, inactiveSeconds; };
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
struct GPUWaterStateCell { float height, velocity, foam, reserved; };
[[vk::binding(8,1)]] StructuredBuffer<GPUWaterPhysicalState> physicalStates;
[[vk::binding(9,1)]] StructuredBuffer<GPUVirtualWaterPage> pages;
[[vk::binding(10,1)]] StructuredBuffer<GPUWaterStateCell> stateCells;
[[vk::binding(11,1)]] Sampler2D minDepthPyramid;

struct Push { uint tier; uint pad0,pad1,pad2; };
[[vk::push_constant]] ConstantBuffer<Push> push;

static const uint InvalidPhysicalPage=0xffffffffu;
static const uint AuthoredWaterFlag=0x10000000u;
static const uint FarAnalyticFlag=0x20000000u;
static const uint StateResolution=16u;
static const uint StateCellsPerPage=StateResolution*StateResolution;
static const float TwoPi=6.28318530718;

float3 octDecode(float2 e){
    float3 n=float3(e.x,1.0-abs(e.x)-abs(e.y),e.y);
    if(n.y<0)n.xz=(1.0-abs(n.zx))*float2(n.x>=0?1:-1,n.z>=0?1:-1);
    return normalize(n);
}
float3 reconstructView(float depth,float2 uv){
    float4 v=mul(float4(uv*2.0-1.0,depth,1.0),frame.inverseProjection);
    return v.xyz/max(abs(v.w),1e-5);
}
float2 projectUv(float3 viewPos,out float depth){
    float4 c=mul(float4(viewPos,1),frame.projection);depth=c.z/c.w;
    return c.xy/c.w*0.5+0.5;
}
float3 reconstructWorld(float3 viewPos){
    float4 w=mul(float4(viewPos,1),frame.inverseView);
    return w.xyz/max(abs(w.w),1e-5);
}
float smithVisibility(float nv,float nl,float rough){
    float a=rough*rough,a2=a*a;
    float lv=nl*sqrt(max((nv-nv*a2)*nv+a2,0));
    float ll=nv*sqrt(max((nl-nl*a2)*nl+a2,0));
    return 0.5/max(lv+ll,1e-4);
}
float3 buildTangent(float3 n){float3 up=abs(n.y)<0.99?float3(0,1,0):float3(0,0,1);return normalize(cross(up,n));}
float3 rotateQuaternion(float3 p,float4 q){return p+2.0*cross(q.xyz,cross(q.xyz,p)+q.w*p);}
float3 inverseRotateQuaternion(float3 p,float4 q){return p+2.0*cross(-q.xyz,cross(-q.xyz,p)+q.w*p);}

GPUWaterStateCell sampleStateGrid(uint physical,float2 uv){
    uv=saturate(uv);float2 p=uv*float(StateResolution-1u);
    uint2 a=uint2(floor(p));uint2 b=min(a+1u,uint2(StateResolution-1u));float2 f=frac(p);
    uint base=physical*StateCellsPerPage;
    GPUWaterStateCell s00=stateCells[base+a.y*StateResolution+a.x];
    GPUWaterStateCell s10=stateCells[base+a.y*StateResolution+b.x];
    GPUWaterStateCell s01=stateCells[base+b.y*StateResolution+a.x];
    GPUWaterStateCell s11=stateCells[base+b.y*StateResolution+b.x];
    GPUWaterStateCell o;
    o.height=lerp(lerp(s00.height,s10.height,f.x),lerp(s01.height,s11.height,f.x),f.y);
    o.velocity=lerp(lerp(s00.velocity,s10.velocity,f.x),lerp(s01.velocity,s11.velocity,f.x),f.y);
    o.foam=lerp(lerp(s00.foam,s10.foam,f.x),lerp(s01.foam,s11.foam,f.x),f.y);o.reserved=0;return o;
}
float2 pageUvFromWorld(GPUVirtualWaterPage page,RendererInstanceData instance,float3 worldPos){
    float3 local=inverseRotateQuaternion(worldPos-instance.positionMaterial.xyz,instance.rotation);
    local/=max(abs(instance.scaleBase.xyz),float3(1e-5));
    return (local.xz-float2(page.minX,page.minZ))/max(float2(page.maxX-page.minX,page.maxZ-page.minZ),float2(1e-5));
}

float gerstnerJacobian(MaterialData material,float2 worldXZ,float time,float cellSize,uint mask){
    float dxx=1.0,dxz=0.0,dzx=0.0,dzz=1.0;
    [unroll]for(uint i=0;i<8;++i){
        if(i>=material.waterWaveCount.x)break;if((mask&(1u<<i))==0u)continue;
        float4 w=material.waterWaves[i];float lod=cellSize>0?smoothstep(2.0,6.0,w.w/cellSize):1.0;
        float2 dir=normalize(w.xy);float k=TwoPi/max(w.w,1e-3);
        float qak=material.waterWaveMotion[i].y*w.z*k*sin(k*dot(dir,worldXZ)+material.waterWaveMotion[i].x*time)*lod;
        dxx-=qak*dir.x*dir.x;dxz-=qak*dir.x*dir.y;dzx-=qak*dir.x*dir.y;dzz-=qak*dir.y*dir.y;
    }
    return dxx*dzz-dxz*dzx;
}

// Hierarchical screen-space ray traversal against a MIN-depth pyramid.
bool traceHierarchical(float3 origin,float3 direction,uint tier,float roughness,float maxDistance,
                       out float2 hitUv,out float hitDepth,out float confidence){
    hitUv=0;hitDepth=1;confidence=0;if(tier==0)return false;
    uint pw,ph,mips;minDepthPyramid.GetDimensions(0,pw,ph,mips);
    uint maxMip=min(mips-1u,tier==3u?6u:(tier==2u?5u:3u));
    uint mip=min(maxMip,uint(roughness*float(maxMip)+0.5));
    uint iterations=tier==3u?40u:(tier==2u?26u:14u);
    float baseStep=maxDistance/float(iterations);
    float t=max(baseStep*0.5,0.04);
    float previousT=0.0;
    [loop]for(uint it=0;it<iterations;++it){
        float3 p=origin+direction*t;float rayDepth;float2 uv=projectUv(p,rayDepth);
        if(any(uv<=0.001)||any(uv>=0.999)||rayDepth<=0.0||rayDepth>=1.0)return false;
        float sceneMin=minDepthPyramid.SampleLevel(uv,float(mip)).r;
        float thickness=0.0008+0.0018*roughness+0.00025*float(mip);
        if(sceneMin<0.99999&&rayDepth>=sceneMin-thickness){
            if(mip>0u){mip--;t=max(previousT,(previousT+t)*0.5);continue;}
            float sceneFull=opaqueDepth.SampleLevel(uv,0).r;
            if(sceneFull<0.99999&&rayDepth>=sceneFull-(0.001+0.0015*roughness)){
                float a=previousT,b=t;
                [unroll]for(uint r=0;r<3;++r){
                    float mid=(a+b)*0.5;float md;float2 muv=projectUv(origin+direction*mid,md);
                    float sd=opaqueDepth.SampleLevel(muv,0).r;
                    if(sd<0.99999&&md>=sd-0.001)b=mid;else a=mid;
                }
                float hd;hitUv=projectUv(origin+direction*b,hd);hitDepth=opaqueDepth.SampleLevel(hitUv,0).r;
                float edge=min(min(hitUv.x,hitUv.y),min(1.0-hitUv.x,1.0-hitUv.y));
                confidence=saturate(edge*12.0)*saturate(1.0-roughness*0.75)*saturate(1.0-b/maxDistance);
                return true;
            }
        }
        previousT=t;
        float scale=exp2(float(mip));
        t+=baseStep*scale;
        if(t>maxDistance)return false;
        if(mip<maxMip)mip++;
    }
    return false;
}

float probeInfluence(ReflectionProbeData probe,float3 worldPosition){
    if((probe.flags&Probe_Enabled)==0u)return 0.0;
    float3 d=abs(worldPosition-probe.positionAndBlendDistance.xyz);float boundary;
    if((probe.flags&Probe_Box)!=0u)boundary=min(min(probe.extentsAndShape.x-d.x,probe.extentsAndShape.y-d.y),probe.extentsAndShape.z-d.z);
    else boundary=probe.extentsAndShape.x-length(d);
    return saturate(boundary/max(probe.positionAndBlendDistance.w,1e-4));
}
float3 boxProjectedReflection(ReflectionProbeData probe,float3 worldPosition,float3 direction){
    if((probe.flags&(Probe_Box|Probe_BoxProject))!=(Probe_Box|Probe_BoxProject))return direction;
    float3 local=worldPosition-probe.positionAndBlendDistance.xyz;float3 safe=direction+sign(direction)*1e-5;
    float3 pos=(probe.extentsAndShape.xyz-local)/safe;float3 neg=(-probe.extentsAndShape.xyz-local)/safe;
    float3 dist=float3(safe.x>0?pos.x:neg.x,safe.y>0?pos.y:neg.y,safe.z>0?pos.z:neg.z);
    float hit=min(min(dist.x,dist.y),dist.z);return normalize(local+direction*max(hit,0.0));
}
ClusterLightRange waterClusterRange(uint2 px,float3 worldPos){
    uint2 grid=uint2((uint(frame.clusteredViewportNearFar.x)+63u)/64u,(uint(frame.clusteredViewportNearFar.y)+63u)/64u);
    float nearP=max(frame.clusteredViewportNearFar.z,1e-4),farP=max(frame.clusteredViewportNearFar.w,nearP+1e-3);
    float viewDepth=max(-mul(float4(worldPos,1),frame.view).z,nearP);
    uint slice=min(uint(log(viewDepth/nearP)/log(farP/nearP)*24.0),23u);
    uint2 tile=min(px/64u,grid-1u);return clusterRanges[(slice*grid.y+tile.y)*grid.x+tile.x];
}
ReflectionCandidate sampleLocalProbes(float3 worldPos,float3 reflection,float roughness,uint mipCount,ClusterLightRange range){
    ReflectionCandidate none={float3(0),0};if(frame.reflectionProbeCount==0u)return none;
    int bestPriority=-2147483647,secondPriority=-2147483647;float bw=0,sw=0;ReflectionProbeData best,second;
    [loop]for(uint c=0;c<range.count;++c){uint encoded=clusterLightIndices[range.offset+c];if((encoded&0x80000000u)==0u)continue;
        uint index=encoded&0x7fffffffu;if(index>=frame.reflectionProbeCount)continue;ReflectionProbeData p=reflectionProbes[index];float w=probeInfluence(p,worldPos);if(w<=0)continue;
        if(p.priority>bestPriority||(p.priority==bestPriority&&w>bw)){second=best;secondPriority=bestPriority;sw=bw;best=p;bestPriority=p.priority;bw=w;}
        else if(p.priority>secondPriority||(p.priority==secondPriority&&w>sw)){second=p;secondPriority=p.priority;sw=w;}}
    float total=bw+sw;if(total<=0)return none;float mip=roughness*float(max(mipCount,1u)-1u);
    float3 value=reflectionProbeTextures[NonUniformResourceIndex(min(best.textureIndex,255u))].SampleLevel(boxProjectedReflection(best,worldPos,reflection),mip).rgb*bw;
    if(sw>0)value+=reflectionProbeTextures[NonUniformResourceIndex(min(second.textureIndex,255u))].SampleLevel(boxProjectedReflection(second,worldPos,reflection),mip).rgb*sw;
    ReflectionCandidate out={value/total,saturate(total)};return out;
}

[numthreads(8,8,1)] [shader("compute")]
void main(uint3 gid:SV_GroupID,uint3 lid:SV_GroupThreadID){
    uint listIndex=push.tier*settings.maxTiles+gid.x;
    uint tile=tileLists[listIndex];uint2 tileXY=uint2(tile%settings.tilesX,tile/settings.tilesX);uint2 px=tileXY*8+lid.xy;
    if(px.x>=settings.width||px.y>=settings.height)return;
    float2 meta=waterMeta.Load(int3(px,0)).xy;uint q=asuint(meta.y);if((q&0x80000000u)==0)return;

    float4 surf=waterSurface.Load(int3(px,0));float2 uv=(float2(px)+0.5)/float2(settings.width,settings.height);
    uint mi=asuint(meta.x);MaterialData m=materials[mi];float3 N=octDecode(surf.xy);float rough=clamp(surf.w,0.045,1.0);
    float3 viewPos=reconstructView(surf.z,uv);float3 worldPos=reconstructWorld(viewPos);float3 V=normalize(frame.cameraPosition.xyz-worldPos);
    const uint pageId=(q>>8u)&0x1fffu;const bool farAnalytic=(q&FarAnalyticFlag)!=0u;const bool authored=(q&AuthoredWaterFlag)!=0u;
    GPUVirtualWaterPage page=(GPUVirtualWaterPage)0;RendererInstanceData instance=(RendererInstanceData)0;
    const bool hasStatePage=!farAnalytic&&(!authored||pageId!=0x1fffu);
    if(hasStatePage){page=pages[pageId];instance=instances[page.instanceIndex];}
    float persistentFoam=0,interaction=0;uint physical=hasStatePage?statePageTable[pageId]:InvalidPhysicalPage;
    if(physical!=InvalidPhysicalPage){float2 stateUv=pageUvFromWorld(page,instance,worldPos);GPUWaterStateCell st=sampleStateGrid(physical,stateUv);GPUWaterPhysicalState summary=physicalStates[physical];persistentFoam=max(st.foam,summary.foam*0.2);interaction=abs(st.height)+abs(st.velocity)*0.1;rough=clamp(rough+interaction*0.12,0.045,1.0);}

    float2 flowVelocity=frame.windDirectionStrength.xz*0.04;
    if(hasStatePage&&page.flowSpeed>1.0e-4){float3 wf=rotateQuaternion(float3(page.flowX,0,page.flowZ),instance.rotation);float2 dir=wf.xz;float dl=length(dir);if(dl>1e-5)flowVelocity=dir/dl*page.flowSpeed*0.04;}
    if(m.waterTextureIndices.z>=0){float2 f=materialTextures[NonUniformResourceIndex(m.waterTextureIndices.z)].SampleLevel(worldPos.xz*0.025,0).xy*2.0-1.0;flowVelocity+=f*0.035;}
    if(m.waterTextureIndices.x>=0){float2 wuv=worldPos.xz*0.15+flowVelocity*frame.windGustFrequencyTime.z;float3 micro=materialTextures[NonUniformResourceIndex(m.waterTextureIndices.x)].SampleLevel(wuv,0).xyz*2.0-1.0;float3 T=buildTangent(N),B=normalize(cross(N,T));float3 microN=normalize(T*micro.x+B*micro.y+N*micro.z);N=normalize(lerp(N,microN,saturate(m.waterFoam.z)*(push.tier>=2?1.0:0.55)));}

    float3 Nvs=normalize(mul(N,(float3x3)frame.view));float3 viewDir=normalize(viewPos);
    uint reflectionTier=((m.waterTextureIndices.w&1)!=0)?(q&3u):0u;uint refractionTier=(q>>2)&3u;
    float3 Rvs=reflect(viewDir,Nvs);float2 ruv;float rdepth,rconfidence;float rdist=reflectionTier==3?140.0:reflectionTier==2?80.0:36.0;
    bool rhit=traceHierarchical(viewPos+Nvs*0.03,Rvs,reflectionTier,rough,rdist,ruv,rdepth,rconfidence);
    float3 worldReflection=reflect(-V,N);uint rw,rh,rm;prefilteredEnvironmentMap.GetDimensions(0,rw,rh,rm);
    ReflectionCandidate rt={float3(0),0};
    ReflectionCandidate ssr={rhit?opaqueColor.SampleLevel(ruv,min(rough*2.0,1.0)).rgb:float3(0),rhit?rconfidence:0.0};
    ReflectionCandidate probe=sampleLocalProbes(worldPos,worldReflection,rough,rm,waterClusterRange(px,worldPos));
    ReflectionCandidate sky={prefilteredEnvironmentMap.SampleLevel(worldReflection,rough*float(max(rm,1u)-1u)).rgb,1.0};
    float3 reflected=resolveReflection(rt,ssr,probe,sky);

    float ior=max(m.waterDeepColorIor.w,1.001);float3 Tvs=refract(viewDir,Nvs,1.0/ior);float2 tuv=uv;float tdepth=1.0,tconf=0;bool thit=false;
    if(refractionTier>=2u)thit=traceHierarchical(viewPos-Nvs*0.03,Tvs,refractionTier,rough,45.0,tuv,tdepth,tconf);
    else if(refractionTier==1u){tuv=clamp(uv+Nvs.xy*m.waterAbsorptionRefraction.w,0.001,0.999);tdepth=opaqueDepth.SampleLevel(tuv,0).r;thit=tdepth<0.99999;}
    float thickness=m.waterScatteringMaxDepth.w;if(thit){float3 hp=reconstructView(tdepth,tuv);thickness=min(length(hp-viewPos),m.waterScatteringMaxDepth.w);}
    float3 sigmaA=max(m.waterAbsorptionRefraction.rgb,0.0),sigmaS=max(m.waterScatteringMaxDepth.rgb,0.0),sigmaT=max(sigmaA+sigmaS,float3(1e-4));
    float3 trans=exp(-sigmaT*thickness);float3 bg=thit?opaqueColor.SampleLevel(tuv,0).rgb:m.waterDeepColorIor.rgb;
    float3 tint=lerp(m.waterShallowColorRoughness.rgb,m.waterDeepColorIor.rgb,saturate(thickness/max(m.waterScatteringMaxDepth.w,1e-3)));
    float3 transmission=bg*trans+(sigmaS/sigmaT)*(1.0-trans)*tint*frame.lightColor.rgb*frame.lightDirectionIntensity.w;

    float nv=saturate(dot(N,V)),f0=pow((ior-1)/(ior+1),2);float3 F=fresnelSchlick(nv,float3(f0));float3 color=lerp(transmission,reflected,F);
    float3 L=normalize(-frame.lightDirectionIntensity.xyz);float nl=saturate(dot(N,L));
    if(nl>0){float3 H=normalize(L+V);float nh=saturate(dot(N,H)),vh=saturate(dot(V,H));float a=rough*rough,a2=a*a;float d=a2/(PBR_PI*pow(nh*nh*(a2-1)+1,2));color+=d*smithVisibility(nv,nl,rough)*fresnelSchlick(vh,float3(f0))*nl*frame.lightColor.rgb*frame.lightDirectionIntensity.w;}

    float jacobianCell=(farAnalytic||authored)?0.0:page.cellSize;
    uint jacobianMask=(farAnalytic||authored)?0xffu:page.waveCandidateMask;
    float jacobian=gerstnerJacobian(m,worldPos.xz,frame.windGustFrequencyTime.z,jacobianCell,jacobianMask);
    if((m.waterTextureIndices.w&2)!=0&&thit&&thickness<10.0){float compression=saturate((1.0-jacobian)*2.5);float focus=min(4.0,1.0/max(abs(jacobian),0.25))-1.0;float receiver=saturate(1.0-thickness/10.0);color+=frame.lightColor.rgb*frame.lightDirectionIntensity.w*compression*focus*receiver*0.025;}

    uint foamTier=(q>>4)&3u;if(foamTier>0){float crest=saturate((m.waterFoam.y-jacobian)*3.0);float foam=saturate(max(crest*(foamTier/3.0),persistentFoam)+interaction*0.25);if(m.waterTextureIndices.y>=0){float2 fuv=worldPos.xz*0.08+flowVelocity*frame.windGustFrequencyTime.z*0.6;foam*=materialTextures[NonUniformResourceIndex(m.waterTextureIndices.y)].SampleLevel(fuv,0).r;}color=lerp(color,float3(0.85,0.92,0.94),foam*m.waterFoam.x);}
    waterLighting[px]=float4(color,1.0);
}
