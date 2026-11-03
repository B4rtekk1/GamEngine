#ifndef GTAO_DIRECTIONS
#define GTAO_DIRECTIONS 3
#endif
#ifndef GTAO_STEPS
#define GTAO_STEPS 3
#endif
#ifndef GTAO_HALF_RES
#define GTAO_HALF_RES 0
#endif

[[vk::binding(0,0)]] Sampler2D horizonDepth;
[[vk::binding(1,0)]] Sampler2D viewNormalTexture;
[[vk::binding(2,0)]] Texture2D<uint> hilbertLut;
[[vk::binding(3,0)]] RWTexture2D<float> rawAo;
[[vk::binding(4,0)]] RWTexture2D<float> guide;
// `centerDepthScale` pulls the centre towards the camera by more than the
// depth pyramid's quantisation, otherwise flat surfaces self-occlude.
struct Settings { float2 projectionScale; float radiusView, centerDepthScale; uint frameIndex, useExternalNormals, maxSampleMip; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;
// The centre and its four neighbours feed edge detection and normal
// reconstruction. Horizon taps reach far beyond any practical apron, so they
// always go through the texture cache instead of shared memory.
groupshared float depthTile[10][10];
static const float BackgroundDepth=65500.0;
static const float Pi=3.14159265359;
float3 positionFromDepth(int2 pixel,float depth,float2 positionScale,float2 positionBias){
 return float3((float2(pixel)+.5)*positionScale+positionBias,-1)*depth;
}
float3 safeNormalize(float3 value){return value*rsqrt(max(dot(value,value),1e-12));}
// Abramowitz & Stegun 4.4.45: |error| <= 6.7e-5 rad, far below what the
// depth pyramid resolves, at a fraction of the cost of acos().
float fastAcos(float x){
 float a=abs(x);
 float r=(((-0.0187293*a+0.0742610)*a-0.2121144)*a+1.5707288)*sqrt(1.0-a);
 return x>=0?r:Pi-r;
}
float2 lowDiscrepancyNoise(uint2 pixel,uint frame){
 const float2 r2=float2(0.754877666,0.569840296);
 uint index=hilbertLut.Load(int3(int2(pixel & 63u),0))+288u*(frame%64u);
 return frac(float(index)*r2+.5);
}

float4 depthEdges(float center,float left,float right,float top,float bottom){
 float4 edges=float4(left,right,top,bottom)-center;
 float slopeLR=(edges.y-edges.x)*.5,slopeTB=(edges.w-edges.z)*.5;
 float4 adjusted=edges+float4(slopeLR,-slopeLR,slopeTB,-slopeTB);
 edges=min(abs(edges),abs(adjusted));
 return saturate(1.25-edges/max(center*.011,1e-4));
}
float3 reconstructNormal(int2 pixel,float center,float left,float right,float top,float bottom,uint2 size,float2 positionScale,float2 positionBias){
 int2 minPixel=int2(0,0),maxPixel=int2(size)-1;
 float3 c=positionFromDepth(pixel,center,positionScale,positionBias);
 float3 l=safeNormalize(positionFromDepth(clamp(pixel+int2(-1,0),minPixel,maxPixel),left,positionScale,positionBias)-c);
 float3 r=safeNormalize(positionFromDepth(clamp(pixel+int2( 1,0),minPixel,maxPixel),right,positionScale,positionBias)-c);
 float3 t=safeNormalize(positionFromDepth(clamp(pixel+int2(0,-1),minPixel,maxPixel),top,positionScale,positionBias)-c);
 float3 b=safeNormalize(positionFromDepth(clamp(pixel+int2(0, 1),minPixel,maxPixel),bottom,positionScale,positionBias)-c);
 float4 edge=depthEdges(center,left,right,top,bottom);
 float4 weight=saturate(float4(edge.x*edge.z,edge.z*edge.y,edge.y*edge.w,edge.w*edge.x)+.01);
 float3 normal=weight.x*cross(l,t)+weight.y*cross(t,r)+weight.z*cross(r,b)+weight.w*cross(b,l);
 normal=safeNormalize(normal);

 if(dot(normal,safeNormalize(-c))<0)normal=-normal;
 return normal;
}
float3 decodeOct(float2 e){float3 n=float3(e,1-abs(e.x)-abs(e.y));if(n.z<0)n.xy=(1-abs(n.yx))*float2(n.x>=0?1:-1,n.y>=0?1:-1);return safeNormalize(n);}

float3 loadViewNormal(int2 pixel,uint2 aoSize){
 uint normalW,normalH;viewNormalTexture.GetDimensions(normalW,normalH);
 float2 uv=(float2(pixel)+.5)/float2(aoSize);
 int2 normalPixel=clamp(int2(uv*float2(normalW,normalH)),int2(0,0),int2(normalW,normalH)-1);
 return decodeOct(viewNormalTexture.Load(int3(normalPixel,0)).xy);
}
// `sinNormal` is sin(normalAngle), shared by both horizons of a slice.
float integrateHorizon(float horizon,float normalAngle,float normalCos,float sinNormal){return(normalCos+2*horizon*sinNormal-cos(2*horizon-normalAngle))*.25;}
[numthreads(8,8,1)] [shader("compute")]
void main(uint3 groupId:SV_GroupID,uint3 threadId:SV_GroupThreadID,uint3 id:SV_DispatchThreadID){
 uint w,h;rawAo.GetDimensions(w,h);int2 size=int2(w,h), base=int2(groupId.xy)*8-1;

 for(uint i=threadId.y*8u+threadId.x;i<100u;i+=64u){int2 t=int2(i%10u,i/10u);int2 q=clamp(base+t,int2(0,0),size-1);depthTile[t.y][t.x]=horizonDepth.Load(int3(q,GTAO_HALF_RES)).r;}
 GroupMemoryBarrierWithGroupSync(); if(any(id.xy>=uint2(w,h)))return;
 int2 local=int2(threadId.xy)+1,p=int2(id.xy);float depth=depthTile[local.y][local.x];
 if(depth>=BackgroundDepth){rawAo[p]=1;guide[p]=1;return;}

 float2 positionScale=2.0/(settings.projectionScale*float2(w,h)),positionBias=-1.0/settings.projectionScale;
 float centerDepth=depth*settings.centerDepthScale;float3 center=positionFromDepth(p,centerDepth,positionScale,positionBias);float left=depthTile[local.y][local.x-1],right=depthTile[local.y][local.x+1],top=depthTile[local.y-1][local.x],bottom=depthTile[local.y+1][local.x];float3 normal=settings.useExternalNormals!=0u?loadViewNormal(p,uint2(w,h)):reconstructNormal(p,depth,left,right,top,bottom,uint2(w,h),positionScale,positionBias);
 float2 localNoise=lowDiscrepancyNoise(uint2(p),settings.frameIndex);
 float visibility=0;

 const uint directions=GTAO_DIRECTIONS,steps=GTAO_STEPS;
 float3 viewDir=normalize(-center);

 const float effectRadius=settings.radiusView*.5*1.457;
 const float effectFalloffRange=.615;
 float2 radiusPixels=.5*effectRadius*abs(settings.projectionScale)/max(centerDepth,1e-4)*float2(w,h);

 const float falloffRange=max(effectFalloffRange*effectRadius,1e-6);
 const float falloffMul=-1.0/falloffRange;
 const float falloffAdd=effectRadius*(1.0-effectFalloffRange)/falloffRange+1.0;
 const float minSampleDistance=1.3/max(max(radiusPixels.x,radiusPixels.y),1e-4);
 // Any non-zero integer tap is at least (1/radiusPixels)*cos(off-axis angle)
 // effect radii away from the centre, whatever depth it reads. Below a
 // quarter pixel every tap therefore has zero falloff weight and cannot move
 // a horizon, so distant pixels skip the texture fetches entirely. The slice
 // integration still runs and yields the same unoccluded value.
 const bool sampleHorizons=max(radiusPixels.x,radiusPixels.y)>=.25;

 visibility+=saturate((10.0-radiusPixels.x)/100.0)*.5;
 [unroll]for(uint d=0;d<directions;++d){

  float angle=(float(d)+localNoise.x)*(Pi/float(directions));
  float sinPhi,cosPhi;sincos(angle,sinPhi,cosPhi);
  float2 ray=float2(cosPhi,-sinPhi);

  float3 screenTangent=float3(cosPhi,sinPhi,0);
  // bitangent is a unit vector orthogonal to viewDir, so their cross product
  // is already normalized.
  float3 bitangent=normalize(cross(viewDir,screenTangent));float3 tangent=cross(bitangent,viewDir);
  float3 projectedNormal=normal-bitangent*dot(normal,bitangent);float normalLength=length(projectedNormal);if(normalLength<1e-4)continue;
  projectedNormal/=normalLength;float normalCos=saturate(dot(projectedNormal,viewDir));float normalAngle=sign(dot(tangent,projectedNormal))*fastAcos(normalCos);
  // cos(normalAngle +/- pi/2) == -/+ sin(normalAngle).
  float sinNormal=sin(normalAngle);
  float lowHorizon[2]={-sinNormal,sinNormal};float horizonCos[2]={lowHorizon[0],lowHorizon[1]};

  if(sampleHorizons){
  [unroll]for(uint step=0;step<steps;++step){
   float stepNoise=frac(localNoise.y+float(d+step*steps)*.61803398875);
   float x=(float(step)+stepNoise)/float(steps);
   float t=x*x+minSampleDistance;
   float2 floatOffset=ray*radiusPixels*t;
   float offsetLength=max(length(floatOffset),1.0);
   float relativeMip=clamp(log2(offsetLength)-3.0,0.0,float(settings.maxSampleMip));
   int2 offset=int2(round(floatOffset));if(!any(offset))continue;
   int2 pixel0=clamp(p+offset,int2(0,0),size-1),pixel1=clamp(p-offset,int2(0,0),size-1);
   // The sampler picks the nearest mip. Taps below one relative level stay
   // on the base level instead of rounding up at 0.5.
   float lod=float(GTAO_HALF_RES)+(relativeMip<1.0?0.0:relativeMip);
   float depth0=horizonDepth.SampleLevel((float2(pixel0)+.5)/float2(size),lod).r;
   float depth1=horizonDepth.SampleLevel((float2(pixel1)+.5)/float2(size),lod).r;
   if(depth0<BackgroundDepth){
    float3 v=positionFromDepth(pixel0,depth0,positionScale,positionBias)-center;float dist2=dot(v,v);
    if(dist2>=1e-6){float invDistance=rsqrt(dist2);float sampleDistance=dist2*invDistance;
     float sampleHorizonCos=dot(v,viewDir)*invDistance;float weight=saturate(sampleDistance*falloffMul+falloffAdd);
     sampleHorizonCos=lerp(lowHorizon[0],sampleHorizonCos,weight);horizonCos[0]=max(horizonCos[0],sampleHorizonCos);}
   }
   if(depth1<BackgroundDepth){
    float3 v=positionFromDepth(pixel1,depth1,positionScale,positionBias)-center;float dist2=dot(v,v);
    if(dist2>=1e-6){float invDistance=rsqrt(dist2);float sampleDistance=dist2*invDistance;
     float sampleHorizonCos=dot(v,viewDir)*invDistance;float weight=saturate(sampleDistance*falloffMul+falloffAdd);
     sampleHorizonCos=lerp(lowHorizon[1],sampleHorizonCos,weight);horizonCos[1]=max(horizonCos[1],sampleHorizonCos);}
   }
  }
  }

  float adjustedNormalLength=lerp(normalLength,1.0,.05);
  float h0=-fastAcos(clamp(horizonCos[1],-1.0,1.0));float h1=fastAcos(clamp(horizonCos[0],-1.0,1.0));
  visibility+=adjustedNormalLength*(integrateHorizon(h0,normalAngle,normalCos,sinNormal)+integrateHorizon(h1,normalAngle,normalCos,sinNormal));
 }

 float4 localEdges=depthEdges(depth,left,right,top,bottom);
 float4 packedEdges=round(saturate(localEdges)*2.9);
 float edgePayload=dot(packedEdges,float4(64.0/255.0,16.0/255.0,4.0/255.0,1.0/255.0));
 rawAo[p]=max(.03,pow(saturate(visibility/float(directions)),2.2));guide[p]=edgePayload;
}
