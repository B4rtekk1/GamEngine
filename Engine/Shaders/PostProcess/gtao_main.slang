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
[[vk::binding(2,0)]] Sampler2D guideDepth;
[[vk::binding(3,0)]] Texture2D<uint> hilbertLut;
[[vk::binding(4,0)]] RWTexture2D<float> rawAo;
[[vk::binding(5,0)]] RWTexture2D<float> guide;
[[vk::binding(6,0)]] RWTexture2D<float> baseDepth;
struct Settings { float2 projectionScale; float radiusView, falloff; uint frameIndex; float padding; uint maxSampleMip; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;
groupshared float depthTile[16][16];
static const float BackgroundDepth=65500.0;
static const float Pi=3.14159265359;
float3 positionFromDepth(int2 pixel,float depth,uint2 size){float2 uv=(float2(pixel)+.5)/size;return float3((uv*2-1)/settings.projectionScale,-1)*depth;}
float3 safeNormalize(float3 value){return value*rsqrt(max(dot(value,value),1e-12));}
float2 lowDiscrepancyNoise(uint2 pixel,uint frame){
 const float2 r2=float2(0.754877666,0.569840296);
 uint index=hilbertLut.Load(int3(int2(pixel & 63u),0))+288u*(frame%64u);
 return frac(float(index)*r2+.5);
}

half4 depthEdges(float center,float left,float right,float top,float bottom){
 half4 edges=half4(half(left),half(right),half(top),half(bottom))-half(center);
 half slopeLR=(edges.y-edges.x)*half(.5),slopeTB=(edges.w-edges.z)*half(.5);
 half4 adjusted=edges+half4(slopeLR,-slopeLR,slopeTB,-slopeTB);
 edges=min(abs(edges),abs(adjusted));
 return saturate(half(1.25)-edges/max(half(center)*half(.011),half(1e-4)));
}
float3 reconstructNormal(int2 pixel,float center,float left,float right,float top,float bottom,uint2 size){
 int2 minPixel=int2(0,0),maxPixel=int2(size)-1;
 float3 c=positionFromDepth(pixel,center,size);
 float3 l=safeNormalize(positionFromDepth(clamp(pixel+int2(-1,0),minPixel,maxPixel),left,size)-c);
 float3 r=safeNormalize(positionFromDepth(clamp(pixel+int2( 1,0),minPixel,maxPixel),right,size)-c);
 float3 t=safeNormalize(positionFromDepth(clamp(pixel+int2(0,-1),minPixel,maxPixel),top,size)-c);
 float3 b=safeNormalize(positionFromDepth(clamp(pixel+int2(0, 1),minPixel,maxPixel),bottom,size)-c);
 half4 edge=depthEdges(center,left,right,top,bottom);
 half4 weight=saturate(half4(edge.x*edge.z,edge.z*edge.y,edge.y*edge.w,edge.w*edge.x)+half(.01));
 float3 normal=weight.x*cross(l,t)+weight.y*cross(t,r)+weight.z*cross(r,b)+weight.w*cross(b,l);
 normal=safeNormalize(normal);

 if(dot(normal,safeNormalize(-c))<0)normal=-normal;
 return normal;
}
float2 encodeOct(float3 n){n/=max(abs(n.x)+abs(n.y)+abs(n.z),1e-5);if(n.z<0)n.xy=(1-abs(n.yx))*float2(n.x>=0?1:-1,n.y>=0?1:-1);return n.xy*.5+.5;}
float3 decodeOct(float2 e){float3 n=float3(e,1-abs(e.x)-abs(e.y));if(n.z<0)n.xy=(1-abs(n.yx))*float2(n.x>=0?1:-1,n.y>=0?1:-1);return safeNormalize(n);}

float3 loadViewNormal(int2 pixel,uint2 aoSize){
 uint normalW,normalH;viewNormalTexture.GetDimensions(normalW,normalH);
 float2 uv=(float2(pixel)+.5)/float2(aoSize);
 int2 normalPixel=clamp(int2(uv*float2(normalW,normalH)),int2(0,0),int2(normalW,normalH)-1);
 return decodeOct(viewNormalTexture.Load(int3(normalPixel,0)).xy);
}
float nearestGuideDepth(int2 pixel,uint2 aoSize){
 uint fullW,fullH;guideDepth.GetDimensions(fullW,fullH);
 uint2 fullSize=uint2(fullW,fullH),p=uint2(pixel);
 uint2 first=p*fullSize/aoSize;
 uint2 last=((p+1u)*fullSize+aoSize-1u)/aoSize;
 float nearest=BackgroundDepth;
 for(uint y=first.y;y<last.y;++y)for(uint x=first.x;x<last.x;++x)
  nearest=min(nearest,guideDepth.Load(int3(uint2(x,y),0)).r);
 return nearest;
}
float integrateHorizon(float horizon,float normalAngle,float normalCos){return(normalCos+2*horizon*sin(normalAngle)-cos(2*horizon-normalAngle))*.25;}
[numthreads(8,8,1)] [shader("compute")]
void main(uint3 groupId:SV_GroupID,uint3 threadId:SV_GroupThreadID,uint3 id:SV_DispatchThreadID){
 uint w,h;rawAo.GetDimensions(w,h);int2 size=int2(w,h), base=int2(groupId.xy)*8-int2(4,4);

 for(uint y=threadId.y;y<16;y+=8) for(uint x=threadId.x;x<16;x+=8){int2 q=clamp(base+int2(x,y),int2(0,0),size-1);float2 uv=(float2(q)+.5)/float2(size);depthTile[y][x]=horizonDepth.SampleLevel(uv,float(GTAO_HALF_RES)).r;}
 GroupMemoryBarrierWithGroupSync(); if(any(id.xy>=uint2(w,h)))return;
 int2 local=int2(threadId.xy)+4,p=int2(id.xy);float depth=depthTile[local.y][local.x];
#if GTAO_HALF_RES
 baseDepth[p]=nearestGuideDepth(p,uint2(w,h));
#endif
 if(depth>=BackgroundDepth){rawAo[p]=1;guide[p]=1;return;}

 float centerDepth=depth*.99999;float3 center=positionFromDepth(p,centerDepth,uint2(w,h));float left=depthTile[local.y][local.x-1],right=depthTile[local.y][local.x+1],top=depthTile[local.y-1][local.x],bottom=depthTile[local.y+1][local.x];float3 normal=settings.padding>0.5?loadViewNormal(p,uint2(w,h)):reconstructNormal(p,depth,left,right,top,bottom,uint2(w,h));
 half2 localNoise=half2(lowDiscrepancyNoise(uint2(p),settings.frameIndex));
 half visibility=0;

 const uint directions=GTAO_DIRECTIONS,steps=GTAO_STEPS;
 float3 viewDir=normalize(-center);

 const float effectRadius=settings.radiusView*.5*1.457;
 const float effectFalloffRange=.615;
 float2 radiusPixels=.5*effectRadius*abs(settings.projectionScale)/max(centerDepth,1e-4)*float2(w,h);

 const float falloffRange=max(effectFalloffRange*effectRadius,1e-6);
 const float falloffMul=-1.0/falloffRange;
 const float falloffAdd=effectRadius*(1.0-effectFalloffRange)/falloffRange+1.0;
 const float minSampleDistance=1.3/max(max(radiusPixels.x,radiusPixels.y),1e-4);

 visibility+=half(saturate((10.0-radiusPixels.x)/100.0))*half(.5);
 [unroll]for(uint d=0;d<directions;++d){

  float angle=(float(d)+localNoise.x)*(Pi/float(directions));
  float sinPhi,cosPhi;sincos(angle,sinPhi,cosPhi);
  float2 ray=float2(cosPhi,-sinPhi);

  float3 screenTangent=float3(cosPhi,sinPhi,0);
  float3 bitangent=normalize(cross(viewDir,screenTangent));float3 tangent=normalize(cross(bitangent,viewDir));
  float3 projectedNormal=normal-bitangent*dot(normal,bitangent);float normalLength=length(projectedNormal);if(normalLength<1e-4)continue;
  projectedNormal/=normalLength;half normalCos=half(saturate(dot(projectedNormal,viewDir)));float normalAngle=sign(dot(tangent,projectedNormal))*acos(float(normalCos));
  half lowHorizon[2]={half(cos(normalAngle+Pi*.5)),half(cos(normalAngle-Pi*.5))};half horizonCos[2]={lowHorizon[0],lowHorizon[1]};

  [unroll]for(uint step=0;step<steps;++step){
   float stepNoise=frac(localNoise.y+float(d+step*steps)*.61803398875);
   float x=(float(step)+stepNoise)/float(steps);
   float t=x*x+minSampleDistance;
   float2 floatOffset=ray*radiusPixels*t;
   float offsetLength2=dot(floatOffset,floatOffset);
   uint relativeMip=uint(offsetLength2>=194.012)+uint(offsetLength2>=776.047)
                   +uint(offsetLength2>=3104.19)+uint(offsetLength2>=12416.75);
   relativeMip=min(relativeMip,settings.maxSampleMip);
   uint selectedMip=uint(GTAO_HALF_RES)+relativeMip;
   int2 offset=int2(round(floatOffset));if(!any(offset))continue;
   int2 pixel0=clamp(p+offset,int2(0,0),size-1),pixel1=clamp(p-offset,int2(0,0),size-1);
   int2 tilePixel0=local+offset,tilePixel1=local-offset;
   bool shared0=relativeMip==0u&&all(tilePixel0>=int2(0,0))&&all(tilePixel0<int2(16,16));
   bool shared1=relativeMip==0u&&all(tilePixel1>=int2(0,0))&&all(tilePixel1<int2(16,16));
   float depth0=shared0?depthTile[tilePixel0.y][tilePixel0.x]:horizonDepth.SampleLevel((float2(pixel0)+.5)/float2(size),float(selectedMip)).r;
   float depth1=shared1?depthTile[tilePixel1.y][tilePixel1.x]:horizonDepth.SampleLevel((float2(pixel1)+.5)/float2(size),float(selectedMip)).r;
   if(depth0<BackgroundDepth){
    float3 v=positionFromDepth(pixel0,depth0,uint2(w,h))-center;float dist2=dot(v,v);
    if(dist2>=1e-6){float invDistance=rsqrt(dist2);float sampleDistance=dist2*invDistance;
     half sampleHorizonCos=half(dot(v,viewDir)*invDistance);half weight=half(saturate(sampleDistance*falloffMul+falloffAdd));
     sampleHorizonCos=lerp(lowHorizon[0],sampleHorizonCos,weight);horizonCos[0]=max(horizonCos[0],sampleHorizonCos);}
   }
   if(depth1<BackgroundDepth){
    float3 v=positionFromDepth(pixel1,depth1,uint2(w,h))-center;float dist2=dot(v,v);
    if(dist2>=1e-6){float invDistance=rsqrt(dist2);float sampleDistance=dist2*invDistance;
     half sampleHorizonCos=half(dot(v,viewDir)*invDistance);half weight=half(saturate(sampleDistance*falloffMul+falloffAdd));
     sampleHorizonCos=lerp(lowHorizon[1],sampleHorizonCos,weight);horizonCos[1]=max(horizonCos[1],sampleHorizonCos);}
   }
  }

  half adjustedNormalLength=half(lerp(normalLength,1.0,.05));
  float h0=-acos(clamp(float(horizonCos[1]),-1.0,1.0));float h1=acos(clamp(float(horizonCos[0]),-1.0,1.0));
  visibility+=adjustedNormalLength*half(integrateHorizon(h0,normalAngle,float(normalCos))+integrateHorizon(h1,normalAngle,float(normalCos)));
 }

 half4 localEdges=depthEdges(depth,left,right,top,bottom);
 float4 packedEdges=round(saturate(float4(localEdges))*2.9);
 float edgePayload=dot(packedEdges,float4(64.0/255.0,16.0/255.0,4.0/255.0,1.0/255.0));
 rawAo[p]=pow(saturate(visibility/float(directions)),2.2);guide[p]=edgePayload;
}
