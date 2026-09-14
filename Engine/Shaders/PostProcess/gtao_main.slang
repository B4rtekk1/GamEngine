// 8x8 output pixels cooperatively cache a 16x16 half-resolution depth tile.
// Ultra quality traces eight directions, two sides and four steps: 64 horizon
// samples per half-resolution pixel. The shared tile keeps those reads local.
[[vk::binding(0,0)]] Sampler2D horizonDepth;
[[vk::binding(1,0)]] Sampler2D viewNormalTexture;
[[vk::binding(2,0)]] RWTexture2D<float> rawAo;
[[vk::binding(3,0)]] RWTexture2D<float4> guide;
[[vk::binding(4,0)]] RWTexture2D<float> baseDepth;
struct Settings { float2 projectionScale; float radiusView, falloff; uint frameIndex; float padding; uint directions, stepsPerDirection, sourceMip, maxSampleMip; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;
groupshared float depthTile[16][16];
static const float BackgroundDepth=65500.0;
static const float Pi=3.14159265359;
float3 positionFromDepth(int2 pixel,float depth,uint2 size){float2 uv=(float2(pixel)+.5)/size;return float3((uv*2-1)/settings.projectionScale,-1)*depth;}
float3 safeNormalize(float3 value){return value*rsqrt(max(dot(value,value),1e-12));}
// XeGTAO uses a deliberately small 64x64 Hilbert pattern.  Keeping the index
// below 4096 preserves every spatial bit when it is converted to float for
// the R2 sequence; a full-screen Hilbert index loses low bits in FP32.
uint hilbertIndex(uint2 pixel){
 uint x=pixel.x&63u,y=pixel.y&63u,index=0;
 for(uint scale=32;scale>0;scale>>=1){
  uint rx=(x&scale)!=0?1:0,ry=(y&scale)!=0?1:0;
  index+=scale*scale*((3*rx)^ry);
  if(ry==0){if(rx!=0){x=(scale*2-1)-x;y=(scale*2-1)-y;}uint swap=x;x=y;y=swap;}
 }
 return index;
}
float2 lowDiscrepancyNoise(uint2 pixel,uint frame){
 const float2 r2=float2(0.754877666,0.569840296);
 uint index=hilbertIndex(pixel)+288u*(frame%64u);
 return frac(float(index)*r2+.5);
}
// XeGTAO's slope-adjusted continuity test.  It rejects the neighbour on the
// other side of a depth discontinuity without rejecting a continuous slope.
float4 depthEdges(float center,float left,float right,float top,float bottom){
 float4 edges=float4(left,right,top,bottom)-center;
 float slopeLR=(edges.y-edges.x)*.5,slopeTB=(edges.w-edges.z)*.5;
 float4 adjusted=edges+float4(slopeLR,-slopeLR,slopeTB,-slopeTB);
 edges=min(abs(edges),abs(adjusted));
 return saturate(1.25-edges/max(center*.011,1e-4));
}
float3 reconstructNormal(int2 pixel,float center,float left,float right,float top,float bottom,uint2 size){
 int2 minPixel=int2(0,0),maxPixel=int2(size)-1;
 float3 c=positionFromDepth(pixel,center,size);
 float3 l=safeNormalize(positionFromDepth(clamp(pixel+int2(-1,0),minPixel,maxPixel),left,size)-c);
 float3 r=safeNormalize(positionFromDepth(clamp(pixel+int2( 1,0),minPixel,maxPixel),right,size)-c);
 float3 t=safeNormalize(positionFromDepth(clamp(pixel+int2(0,-1),minPixel,maxPixel),top,size)-c);
 float3 b=safeNormalize(positionFromDepth(clamp(pixel+int2(0, 1),minPixel,maxPixel),bottom,size)-c);
 float4 edge=depthEdges(center,left,right,top,bottom);
 float4 weight=saturate(float4(edge.x*edge.z,edge.z*edge.y,edge.y*edge.w,edge.w*edge.x)+.01);
 float3 normal=weight.x*cross(l,t)+weight.y*cross(t,r)+weight.z*cross(r,b)+weight.w*cross(b,l);
 normal=safeNormalize(normal);
 // positionFromDepth uses negative view-space Z in front of the camera.
 // Orient the reconstructed normal toward the camera independently of the
 // neighbour winding, which was originally derived for positive-Z depth.
 if(dot(normal,safeNormalize(-c))<0)normal=-normal;
 return normal;
}
float2 encodeOct(float3 n){n/=max(abs(n.x)+abs(n.y)+abs(n.z),1e-5);if(n.z<0)n.xy=(1-abs(n.yx))*float2(n.x>=0?1:-1,n.y>=0?1:-1);return n.xy*.5+.5;}
float3 decodeOct(float2 e){float3 n=float3(e,1-abs(e.x)-abs(e.y));if(n.z<0)n.xy=(1-abs(n.yx))*float2(n.x>=0?1:-1,n.y>=0?1:-1);return safeNormalize(n);}
// The prepass normal buffer is always native-resolution, while Low and
// Medium GTAO run at half resolution.  Map through normalized coordinates
// rather than using the AO pixel directly.
float3 loadViewNormal(int2 pixel,uint2 aoSize){
 uint normalW,normalH;viewNormalTexture.GetDimensions(normalW,normalH);
 float2 uv=(float2(pixel)+.5)/float2(aoSize);
 int2 normalPixel=clamp(int2(uv*float2(normalW,normalH)),int2(0,0),int2(normalW,normalH)-1);
 return decodeOct(viewNormalTexture.Load(int3(normalPixel,0)).xy);
}
// Integral of the projected cosine over one unoccluded horizon arc.
float integrateHorizon(float horizon,float normalAngle,float normalCos){return(normalCos+2*horizon*sin(normalAngle)-cos(2*horizon-normalAngle))*.25;}
[numthreads(8,8,1)] [shader("compute")]
void main(uint3 groupId:SV_GroupID,uint3 threadId:SV_GroupThreadID,uint3 id:SV_DispatchThreadID){
 uint w,h;rawAo.GetDimensions(w,h);int2 size=int2(w,h), base=int2(groupId.xy)*8-int2(4,4);
 // Cache the exact selected-mip value that a non-shared horizon sample would
 // read.  The depth pyramid's filter is deliberately not min(2x2), so
 // rebuilding it here would make AO depend on workgroup boundaries.
 for(uint y=threadId.y;y<16;y+=8) for(uint x=threadId.x;x<16;x+=8){int2 q=clamp(base+int2(x,y),int2(0,0),size-1);float2 uv=(float2(q)+.5)/float2(size);depthTile[y][x]=horizonDepth.SampleLevel(uv,float(settings.sourceMip)).r;}
 GroupMemoryBarrierWithGroupSync(); if(any(id.xy>=uint2(w,h)))return;
 int2 local=int2(threadId.xy)+4,p=int2(id.xy);float depth=depthTile[local.y][local.x];baseDepth[p]=depth;if(depth>=BackgroundDepth){rawAo[p]=1;guide[p]=float4(0,1,1,1);return;}
 // Keep edge detection on the original depth, but move the AO center a tiny
 // amount toward the camera to prevent FP32 depth quantization self-occlusion.
 float centerDepth=depth*.99999;float3 center=positionFromDepth(p,centerDepth,uint2(w,h));float left=depthTile[local.y][local.x-1],right=depthTile[local.y][local.x+1],top=depthTile[local.y-1][local.x],bottom=depthTile[local.y+1][local.x];float3 normal=settings.padding>0.5?loadViewNormal(p,uint2(w,h)):reconstructNormal(p,depth,left,right,top,bottom,uint2(w,h));
 float2 localNoise=lowDiscrepancyNoise(uint2(p),settings.frameIndex);
 float visibility=0;
 // The shared tile is used only for the source mip.  Horizon samples may
 // select a coarser mip based on their offset, so reading them from the tile
 // would make their depth depend on the workgroup boundary.
 const uint directions=max(settings.directions,1u),steps=min(max(settings.stepsPerDirection,1u),4u);
 float3 viewDir=normalize(-center);
 // Intel's defaults are EffectRadius 0.5 multiplied by RadiusMultiplier
 // 1.457, and EffectFalloffRange 0.615.
 const float effectRadius=settings.radiusView*.5*1.457;
 const float effectFalloffRange=.615;
 float2 radiusPixels=.5*effectRadius*abs(settings.projectionScale)/max(centerDepth,1e-4)*float2(w,h);
 // XeGTAO's linear falloff band starts at 38.5% of the effect radius rather
 // than applying a quadratic attenuation across the entire horizon ray.
 const float falloffRange=max(effectFalloffRange*effectRadius,1e-6);
 const float falloffMul=-1.0/falloffRange;
 const float falloffAdd=effectRadius*(1.0-effectFalloffRange)/falloffRange+1.0;
 const float minSampleDistance=1.3/max(max(radiusPixels.x,radiusPixels.y),1e-4);
 // Match XeGTAO's small-radius fade.  Integer-rounded offsets otherwise
 // cause abrupt visibility changes when the radius spans only a few pixels.
 visibility+=saturate((10.0-radiusPixels.x)/100.0)*.5;
 for(uint d=0;d<directions;++d){
  // Jitter slices and radial locations independently.  The squared
  // distribution places more samples where near-field occluders matter most.
  float angle=(float(d)+localNoise.x)*(Pi/float(directions));
  float cosPhi=cos(angle),sinPhi=sin(angle);
  // Vulkan pixel coordinates grow downward, whereas view-space Y grows up.
  float2 ray=float2(cosPhi,-sinPhi);
  // radiusPixels already converts the view-space radius into pixels; applying
  // the projection scale here again would rotate the slice plane relative to
  // the actual view-space sampling direction.
  float3 screenTangent=float3(cosPhi,sinPhi,0);
  float3 bitangent=normalize(cross(viewDir,screenTangent));float3 tangent=normalize(cross(bitangent,viewDir));
  float3 projectedNormal=normal-bitangent*dot(normal,bitangent);float normalLength=length(projectedNormal);if(normalLength<1e-4)continue;
  projectedNormal/=normalLength;float normalCos=saturate(dot(projectedNormal,viewDir));float normalAngle=sign(dot(tangent,projectedNormal))*acos(normalCos);
  float lowHorizon[2]={cos(normalAngle+Pi*.5),cos(normalAngle-Pi*.5)};float horizonCos[2]={lowHorizon[0],lowHorizon[1]};
  for(uint side=0;side<2;++side)for(uint step=0;step<steps;++step){
   float stepNoise=frac(localNoise.y+float(d+step*steps)*.61803398875);
   float t=pow((float(step)+stepNoise)/float(steps),2)+minSampleDistance;
   float2 floatOffset=ray*radiusPixels*(side==0?1.0:-1.0)*t;
   float offsetLength=length(floatOffset);
   float sampleMip=float(settings.sourceMip)+min(max(log2(max(offsetLength,1.0))-3.30,0.0),float(settings.maxSampleMip));
   int2 offset=int2(round(floatOffset));if(!any(offset))continue;
   int2 samplePixel=clamp(p+offset,int2(0,0),size-1),tilePixel=local+offset;
   uint selectedMip=uint(round(sampleMip));
   bool canUseShared=selectedMip==settings.sourceMip&&all(tilePixel>=int2(0,0))&&all(tilePixel<int2(16,16));
   float sample=canUseShared?depthTile[tilePixel.y][tilePixel.x]:horizonDepth.SampleLevel((float2(samplePixel)+.5)/float2(size),sampleMip).r;
   if(sample>=BackgroundDepth)continue;
   float3 v=positionFromDepth(samplePixel,sample,uint2(w,h))-center;float dist2=dot(v,v);if(dist2<1e-6)continue;
   float sampleDistance=sqrt(dist2);
   float sampleHorizonCos=dot(v/sampleDistance,viewDir);
   float weight=saturate(sampleDistance*falloffMul+falloffAdd);
   sampleHorizonCos=lerp(lowHorizon[side],sampleHorizonCos,weight);
   horizonCos[side]=max(horizonCos[side],sampleHorizonCos);
  }
  // XeGTAO keeps these horizon angles unclamped.  The projected-normal
  // correction counters its slight high-slope over-darkening instead.
  normalLength=lerp(normalLength,1.0,.05);
  float h0=-acos(clamp(horizonCos[1],-1.0,1.0));float h1=acos(clamp(horizonCos[0],-1.0,1.0));
  visibility+=normalLength*(integrateHorizon(h0,normalAngle,normalCos)+integrateHorizon(h1,normalAngle,normalCos));
 }
 // Preserve directional local continuity for the denoiser.  Horizon samples
 // are deliberately excluded: a distant wall is not an edge of this pixel.
 float4 localEdges=depthEdges(depth,left,right,top,bottom);
 float4 packedEdges=round(saturate(localEdges)*2.9);
 float edgePayload=dot(packedEdges,float4(64.0/255.0,16.0/255.0,4.0/255.0,1.0/255.0));
 rawAo[p]=pow(saturate(visibility/float(directions)),2.2);guide[p]=float4(encodeOct(normal),edgePayload,0);
}
