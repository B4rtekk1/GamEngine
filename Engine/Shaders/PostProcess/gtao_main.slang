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
float2 encodeOct(float3 n){n/=max(abs(n.x)+abs(n.y)+abs(n.z),1e-5);if(n.z<0)n.xy=(1-abs(n.yx))*float2(n.x>=0?1:-1,n.y>=0?1:-1);return n.xy*.5+.5;}
// Integral of the projected cosine over one unoccluded horizon arc.
float integrateHorizon(float horizon,float normalAngle,float normalCos){return(normalCos+2*horizon*sin(normalAngle)-cos(2*horizon-normalAngle))*.25;}
[numthreads(8,8,1)] [shader("compute")]
void main(uint3 groupId:SV_GroupID,uint3 threadId:SV_GroupThreadID,uint3 id:SV_DispatchThreadID){
 uint w,h;rawAo.GetDimensions(w,h);int2 size=int2(w,h), base=int2(groupId.xy)*8-int2(4,4);
 for(uint y=threadId.y;y<16;y+=8) for(uint x=threadId.x;x<16;x+=8){int2 p=clamp(base+int2(x,y),int2(0,0),size-1);depthTile[y][x]=halfDepth.SampleLevel((float2(p)+.5)/float2(size),settings.sourceMip).r;}
 GroupMemoryBarrierWithGroupSync(); if(any(id.xy>=uint2(w,h)))return;
 int2 local=int2(threadId.xy)+4,p=int2(id.xy);float depth=depthTile[local.y][local.x];if(depth>=BackgroundDepth){rawAo[p]=1;guide[p]=float4(BackgroundDepth,0,1,1);return;}
 float3 center=positionFromDepth(p,depth,uint2(w,h));float dx=depthTile[local.y][local.x+1],dy=depthTile[local.y+1][local.x];float3 normal=normalize(cross(positionFromDepth(p+int2(1,0),dx,uint2(w,h))-center,positionFromDepth(p+int2(0,1),dy,uint2(w,h))-center));
 // The spatial hash removes structured bands. Advancing by the golden angle
 // each frame makes the slices a temporal sampling sequence.
 float spatialRotation=frac(sin(dot(float2(p),float2(12.9898,78.233)))*43758.5453)*6.2831853;
 float rotation=spatialRotation+float(settings.frameIndex)*2.39996323;
 float visibility=0,edge=0;
 // The tile covers nearby taps; projected-radius taps beyond its four-pixel
 // apron fall back to the source texture below.
 const uint directions=max(settings.directions,1u),steps=min(max(settings.stepsPerDirection,1u),4u);
 float3 viewDir=normalize(-center);
 float2 radiusPixels=.5*settings.radiusView*abs(settings.projectionScale)/max(depth,1e-4)*float2(w,h);
 for(uint d=0;d<directions;++d){
  float angle=rotation+float(d)*(Pi/float(directions));float2 ray=float2(cos(angle),sin(angle));
  float3 screenTangent=normalize(float3(ray.x/settings.projectionScale.x,ray.y/settings.projectionScale.y,0));
  float3 bitangent=normalize(cross(viewDir,screenTangent));float3 tangent=normalize(cross(bitangent,viewDir));
  float3 projectedNormal=normal-bitangent*dot(normal,bitangent);float normalLength=length(projectedNormal);if(normalLength<1e-4)continue;
  projectedNormal/=normalLength;float normalCos=saturate(dot(projectedNormal,viewDir));float normalAngle=sign(dot(tangent,projectedNormal))*acos(normalCos);
  float lowHorizon[2]={cos(normalAngle+Pi*.5),cos(normalAngle-Pi*.5)};float horizonCos[2]={lowHorizon[0],lowHorizon[1]};
  for(uint side=0;side<2;++side)for(uint step=1;step<=steps;++step){
   float t=(float(step)-.5)/float(steps);int2 offset=int2(round(ray*radiusPixels*(side==0?1.0:-1.0)*t));if(!any(offset))continue;
   int2 samplePixel=clamp(p+offset,int2(0,0),size-1),tilePixel=local+offset;
   float sampleMip=float(settings.sourceMip)+min(max(log2(max(length(float2(offset)),1.0))-1.0,0.0),float(settings.maxSampleMip));
   float sample=all(tilePixel>=int2(0,0))&&all(tilePixel<int2(16,16))?depthTile[tilePixel.y][tilePixel.x]:halfDepth.SampleLevel((float2(samplePixel)+.5)/float2(size),sampleMip).r;
   edge=max(edge,abs(sample-depth)/max(depth,1e-3));if(sample>=BackgroundDepth)continue;
   float3 v=positionFromDepth(samplePixel,sample,uint2(w,h))-center;float dist2=dot(v,v);if(dist2<1e-6)continue;
   float3 sliceV=v-bitangent*dot(v,bitangent);float sliceLength=length(sliceV);if(sliceLength<1e-5)continue;
   float sampleHorizonCos=dot(sliceV/sliceLength,viewDir);float attenuation=saturate(1-dist2/(settings.radiusView*settings.radiusView));
   horizonCos[side]=max(horizonCos[side],lerp(lowHorizon[side],sampleHorizonCos,attenuation));
  }
  float h0=-acos(clamp(horizonCos[1],-1.0,1.0));float h1=acos(clamp(horizonCos[0],-1.0,1.0));
  h0=normalAngle+clamp(h0-normalAngle,-Pi*.5,Pi*.5);h1=normalAngle+clamp(h1-normalAngle,-Pi*.5,Pi*.5);
  visibility+=normalLength*(integrateHorizon(h0,normalAngle,normalCos)+integrateHorizon(h1,normalAngle,normalCos));
 }
 rawAo[p]=saturate(visibility/float(directions));guide[p]=float4(depth,encodeOct(normal),saturate(edge*settings.falloff));
}
