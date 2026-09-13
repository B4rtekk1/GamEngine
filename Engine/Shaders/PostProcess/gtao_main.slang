// 8x8 output pixels cooperatively cache a 16x16 half-resolution depth tile.
// Ultra quality traces eight directions, two sides and four steps: 64 horizon
// samples per half-resolution pixel. The shared tile keeps those reads local.
[[vk::binding(0,0)]] Sampler2D halfDepth;
[[vk::binding(1,0)]] RWTexture2D<float> rawAo;
[[vk::binding(2,0)]] RWTexture2D<float4> guide;
struct Settings { float2 projectionScale; float radiusView, falloff; uint frameIndex; float padding; uint directions, stepsPerDirection; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;
groupshared float depthTile[16][16];
static const float BackgroundDepth=65500.0;
float3 positionFromDepth(int2 pixel,float depth,uint2 size){float2 uv=(float2(pixel)+.5)/size;return float3((uv*2-1)/settings.projectionScale,-1)*depth;}
float2 encodeOct(float3 n){n/=max(abs(n.x)+abs(n.y)+abs(n.z),1e-5);if(n.z<0)n.xy=(1-abs(n.yx))*float2(n.x>=0?1:-1,n.y>=0?1:-1);return n.xy*.5+.5;}
[numthreads(8,8,1)] [shader("compute")]
void main(uint3 groupId:SV_GroupID,uint3 threadId:SV_GroupThreadID,uint3 id:SV_DispatchThreadID){
 uint w,h;rawAo.GetDimensions(w,h);int2 size=int2(w,h), base=int2(groupId.xy)*8-int2(4,4);
 for(uint y=threadId.y;y<16;y+=8) for(uint x=threadId.x;x<16;x+=8){int2 p=clamp(base+int2(x,y),int2(0,0),size-1);depthTile[y][x]=halfDepth.Load(int3(p,0)).r;}
 GroupMemoryBarrierWithGroupSync(); if(any(id.xy>=uint2(w,h)))return;
 int2 local=int2(threadId.xy)+4,p=int2(id.xy);float depth=depthTile[local.y][local.x];if(depth>=BackgroundDepth){rawAo[p]=1;guide[p]=float4(BackgroundDepth,0,1,1);return;}
 float3 center=positionFromDepth(p,depth,uint2(w,h));float dx=depthTile[local.y][local.x+1],dy=depthTile[local.y+1][local.x];float3 normal=normalize(cross(positionFromDepth(p+int2(1,0),dx,uint2(w,h))-center,positionFromDepth(p+int2(0,1),dy,uint2(w,h))-center));
 // The spatial hash removes structured bands.  Advancing by the golden angle
 // each frame makes the four directions a temporal sampling sequence rather
 // than re-evaluating the same rays every frame.
 float spatialRotation=frac(sin(dot(float2(p),float2(12.9898,78.233)))*43758.5453)*6.2831853;
 float rotation=spatialRotation+float(settings.frameIndex)*2.39996323;
 float occlusion=0,edge=0;
 const uint directions=max(settings.directions,1u),steps=max(settings.stepsPerDirection,1u);
 for(uint d=0;d<directions;++d)for(int side=-1;side<=1;side+=2)for(uint step=1;step<=steps;++step){float angle=rotation+float(d)*(3.14159265/float(directions))+float(side<0)*3.14159265;int2 o=int2(round(float2(cos(angle),sin(angle))*float(step)));float sample=depthTile[local.y+o.y][local.x+o.x];edge=max(edge,abs(sample-depth)/max(depth,1e-3));if(sample>=BackgroundDepth)continue;float3 v=positionFromDepth(p+o,sample,uint2(w,h))-center;float dist2=dot(v,v);float range=saturate(1-dist2/(settings.radiusView*settings.radiusView));occlusion+=saturate(dot(normalize(v),normal))*range;}
 rawAo[p]=saturate(1-occlusion/float(directions*steps*2));guide[p]=float4(depth,encodeOct(normal),saturate(edge*settings.falloff));
}
