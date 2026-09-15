import Common.SceneData;

[[vk::binding(1,0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(2,0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(5,0)]] StructuredBuffer<RendererInstanceData> instances;

struct GPUFarOceanBody { uint instanceIndex; float startDistance; float normalCellSize; uint waveMask; };
struct GPUFarOceanConfig { uint bodyCount,padding0,padding1,padding2; GPUFarOceanBody bodies[16]; };
[[vk::binding(0,1)]] ConstantBuffer<GPUFarOceanConfig> config;

static const float TwoPi=6.28318530718;
static const uint FarAnalyticPageId=0x1fffu;
static const uint FarAnalyticFlag=0x20000000u;

float3 rotateQuaternion(float3 p,float4 q){return p+2.0*cross(q.xyz,cross(q.xyz,p)+q.w*p);}
float2 octEncode(float3 n){n/=abs(n.x)+abs(n.y)+abs(n.z);float2 e=n.xz;if(n.y<0)e=(1.0-abs(e.yx))*float2(e.x>=0?1:-1,e.y>=0?1:-1);return e;}

float3 rayDirection(float2 uv){
    float4 view=mul(float4(uv*2.0-1.0,1.0,1.0),frame.inverseProjection);
    float3 viewDir=normalize(view.xyz/max(abs(view.w),1e-5));
    return normalize(mul(float4(viewDir,0.0),frame.inverseView).xyz);
}

void evaluateFarNormal(MaterialData m,float2 worldXZ,float time,float cell,uint mask,out float3 normal,out float variance){
    float3 t=float3(1,0,0),b=float3(0,0,1);variance=0.0;
    [unroll]for(uint i=0;i<8;++i){
        if(i>=m.waterWaveCount.x)break;float4 w=m.waterWaves[i];float2 dir=normalize(w.xy);float k=TwoPi/max(w.w,1e-3);
        float geom=(mask&(1u<<i))!=0?smoothstep(2.0,6.0,w.w/cell):0.0;
        float s,c;sincos(k*dot(dir,worldXZ)+m.waterWaveMotion[i].x*time,s,c);
        float h=m.waterWaveMotion[i].y*w.z;
        t+=geom*float3(-h*dir.x*dir.x*k*s,w.z*dir.x*k*c,-h*dir.x*dir.y*k*s);
        b+=geom*float3(-h*dir.x*dir.y*k*s,w.z*dir.y*k*c,-h*dir.y*dir.y*k*s);
        float slope=w.z*k;variance+=0.5*(1.0-geom*geom)*slope*slope;
    }
    normal=normalize(cross(b,t));
}

struct VSOut{float4 position:SV_Position;[[vk::location(0)]] float2 uv;};
[shader("vertex")]
VSOut vertexMain(uint id:SV_VertexID){float2 p=id==0?float2(-1,-1):id==1?float2(3,-1):float2(-1,3);VSOut o;o.position=float4(p,0,1);o.uv=p*0.5+0.5;return o;}

struct PSOut{
 [[vk::location(0)]] float4 surface:SV_Target0;
 [[vk::location(1)]] float2 meta:SV_Target1;
 [[vk::location(2)]] float2 velocity:SV_Target2;
 float depth:SV_Depth;
};

[shader("fragment")]
PSOut fragmentMain(VSOut i){
    float3 ro=frame.cameraPosition.xyz,rd=rayDirection(i.uv);float bestT=1e30;uint bestBody=0xffffffffu;float3 bestPos=0;float3 bestNormal=float3(0,1,0);float bestVariance=0;uint bestMaterial=0;
    [loop]for(uint body=0;body<config.bodyCount;++body){
        GPUFarOceanBody far=config.bodies[body];RendererInstanceData inst=instances[far.instanceIndex];
        float3 planeN=normalize(rotateQuaternion(float3(0,1,0),inst.rotation));float denom=dot(rd,planeN);if(abs(denom)<1e-5)continue;
        float t=dot(inst.positionMaterial.xyz-ro,planeN)/denom;if(t<=far.startDistance||t>=bestT)continue;
        float3 wp=ro+rd*t;uint mi=asuint(inst.positionMaterial.w);MaterialData m=materials[mi];float3 n;float var;
        evaluateFarNormal(m,wp.xz,frame.windGustFrequencyTime.z,far.normalCellSize,far.waveMask,n,var);
        bestT=t;bestBody=body;bestPos=wp;bestNormal=rotateQuaternion(n,inst.rotation);bestVariance=var;bestMaterial=mi;
    }
    if(bestBody==0xffffffffu)discard;
    float4 clip=mul(mul(float4(bestPos,1),frame.view),frame.projection);float4 prev=mul(mul(float4(bestPos,1),frame.previousView),frame.previousProjection);
    float rough=clamp(sqrt(materials[bestMaterial].waterShallowColorRoughness.w*materials[bestMaterial].waterShallowColorRoughness.w+2.0*bestVariance),0.045,1.0);
    uint packed=1u | (0u<<2u) | (0u<<4u) | (FarAnalyticPageId<<8u) | FarAnalyticFlag | 0x40000000u | 0x80000000u;
    PSOut o;o.surface=float4(octEncode(normalize(bestNormal)),clip.z/clip.w,rough);o.meta=float2(asfloat(bestMaterial),asfloat(packed));o.velocity=(clip.xy/clip.w-prev.xy/prev.w)*0.5;o.depth=clip.z/clip.w;return o;
}
