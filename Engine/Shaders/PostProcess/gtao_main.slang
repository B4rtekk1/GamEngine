// 8x8 output pixels cooperatively cache a 16x16 half-resolution depth tile.
// Ultra quality traces eight directions, two sides and four steps: 64 horizon
// samples per half-resolution pixel. The shared tile keeps those reads local.
[[vk::binding(0,0)]] Sampler2D halfDepth;
[[vk::binding(1,0)]] RWTexture2D<float> rawAo;
[[vk::binding(2,0)]] RWTexture2D<float4> guide;
struct Settings { float2 projectionScale; float radiusView, falloff; uint frameIndex; float padding; uint directions, stepsPerDirection, sourceMip, maxSampleMip; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;
groupshared float depthTile[16][16];
static const float BackgroundDepth=65500.0;
static const float Pi=3.14159265359;
float3 positionFromDepth(int2 pixel,float depth,uint2 size){float2 uv=(float2(pixel)+.5)/size;return float3((uv*2-1)/settings.projectionScale,-1)*depth;}
float3 safeNormalize(float3 value){return value*rsqrt(max(dot(value,value),1e-12));}
// Spatially coherent Hilbert indexing plus the R2 sequence provides a
// low-discrepancy two-dimensional sample sequence without a noise texture.
uint hilbertIndex(uint2 pixel){
 uint x=pixel.x,y=pixel.y,index=0;
 for(uint scale=32768;scale>0;scale>>=1){
  uint rx=(x&scale)!=0?1:0,ry=(y&scale)!=0?1:0;
  index+=scale*scale*((3*rx)^ry);
  if(ry==0){if(rx!=0){x=(scale*2-1)-x;y=(scale*2-1)-y;}uint swap=x;x=y;y=swap;}
 }
 return index;
}
float2 lowDiscrepancyNoise(uint2 pixel,uint frame){
 const float2 r2=float2(0.754877666,0.569840296);
 return frac(float(hilbertIndex(pixel))*r2+(float(frame)+.5)*r2);
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
// Integral of the projected cosine over one unoccluded horizon arc.
float integrateHorizon(float horizon,float normalAngle,float normalCos){return(normalCos+2*horizon*sin(normalAngle)-cos(2*horizon-normalAngle))*.25;}
[numthreads(8,8,1)] [shader("compute")]
void main(uint3 groupId:SV_GroupID,uint3 threadId:SV_GroupThreadID,uint3 id:SV_DispatchThreadID){
 uint w,h;rawAo.GetDimensions(w,h);int2 size=int2(w,h), base=int2(groupId.xy)*8-int2(4,4);
 for(uint y=threadId.y;y<16;y+=8) for(uint x=threadId.x;x<16;x+=8){int2 p=clamp(base+int2(x,y),int2(0,0),size-1);depthTile[y][x]=halfDepth.SampleLevel((float2(p)+.5)/float2(size),settings.sourceMip).r;}
 GroupMemoryBarrierWithGroupSync(); if(any(id.xy>=uint2(w,h)))return;
 int2 local=int2(threadId.xy)+4,p=int2(id.xy);float depth=depthTile[local.y][local.x];if(depth>=BackgroundDepth){rawAo[p]=1;guide[p]=float4(0,1,1,1);return;}
 float3 center=positionFromDepth(p,depth,uint2(w,h));float left=depthTile[local.y][local.x-1],right=depthTile[local.y][local.x+1],top=depthTile[local.y-1][local.x],bottom=depthTile[local.y+1][local.x];float3 normal=reconstructNormal(p,depth,left,right,top,bottom,uint2(w,h));
 float2 localNoise=lowDiscrepancyNoise(uint2(p),settings.frameIndex);
 float visibility=0;
 // The tile covers nearby taps; projected-radius taps beyond its four-pixel
 // apron fall back to the source texture below.
 const uint directions=max(settings.directions,1u),steps=min(max(settings.stepsPerDirection,1u),4u);
 float3 viewDir=normalize(-center);
 float2 radiusPixels=.5*settings.radiusView*abs(settings.projectionScale)/max(depth,1e-4)*float2(w,h);
 const float minSampleDistance=1.3/max(max(radiusPixels.x,radiusPixels.y),1e-4);
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
   int2 offset=int2(round(ray*radiusPixels*(side==0?1.0:-1.0)*t));if(!any(offset))continue;
   int2 samplePixel=clamp(p+offset,int2(0,0),size-1),tilePixel=local+offset;
   float sampleMip=float(settings.sourceMip)+min(max(log2(max(length(float2(offset)),1.0))-3.15,0.0),float(settings.maxSampleMip));
   float sample=all(tilePixel>=int2(0,0))&&all(tilePixel<int2(16,16))?depthTile[tilePixel.y][tilePixel.x]:halfDepth.SampleLevel((float2(samplePixel)+.5)/float2(size),sampleMip).r;
   if(sample>=BackgroundDepth)continue;
   float3 v=positionFromDepth(samplePixel,sample,uint2(w,h))-center;float dist2=dot(v,v);if(dist2<1e-6)continue;
   float3 sliceV=v-bitangent*dot(v,bitangent);float sliceLength=length(sliceV);if(sliceLength<1e-5)continue;
   float sampleHorizonCos=dot(sliceV/sliceLength,viewDir);float attenuation=saturate(1-dist2/(settings.radiusView*settings.radiusView));
   horizonCos[side]=max(horizonCos[side],lerp(lowHorizon[side],sampleHorizonCos,attenuation));
  }
  float h0=-acos(clamp(horizonCos[1],-1.0,1.0));float h1=acos(clamp(horizonCos[0],-1.0,1.0));
  h0=normalAngle+clamp(h0-normalAngle,-Pi*.5,Pi*.5);h1=normalAngle+clamp(h1-normalAngle,-Pi*.5,Pi*.5);
  visibility+=normalLength*(integrateHorizon(h0,normalAngle,normalCos)+integrateHorizon(h1,normalAngle,normalCos));
 }
 // Preserve directional local continuity for the denoiser.  Horizon samples
 // are deliberately excluded: a distant wall is not an edge of this pixel.
 float4 localEdges=depthEdges(depth,left,right,top,bottom);
 float4 packedEdges=round(saturate(localEdges)*2.9);
 float edgePayload=dot(packedEdges,float4(64.0/255.0,16.0/255.0,4.0/255.0,1.0/255.0));
 rawAo[p]=saturate(visibility/float(directions));guide[p]=float4(encodeOct(normal),edgePayload,0);
}
