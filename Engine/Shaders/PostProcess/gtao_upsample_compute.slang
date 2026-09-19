[[vk::binding(0,0)]] Sampler2D halfAo;
[[vk::binding(1,0)]] Sampler2D linearDepth;
[[vk::binding(2,0)]] Sampler2D baseDepth;
[[vk::binding(3,0)]] RWTexture2D<float> fullAo;
// `inputToOutputScale` is input resolution / output resolution. Multiplying
// an output pixel coordinate by it maps the bilateral taps into input space.
struct Settings { float2 inputToOutputScale; float depthSigma; uint padding; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;
// An 8x8 output group needs a 6x6 source region at half resolution and a
// 9x9 region for native-resolution GTAO, both including the bilinear border.
groupshared float aoTile[9][9];
groupshared float depthTile[9][9];
[numthreads(8,8,1)] [shader("compute")]
void main(uint3 groupId:SV_GroupID,uint3 threadId:SV_GroupThreadID,uint3 id:SV_DispatchThreadID){
 uint hw,hh;halfAo.GetDimensions(hw,hh);
 int2 tileBase=int2(floor((float2(groupId.xy*8)+.5)*settings.inputToOutputScale-.5));
 int2 tileSize=int2(settings.inputToOutputScale.x>.75?9:6,settings.inputToOutputScale.y>.75?9:6);
 uint loadIndex=threadId.y*8+threadId.x;
 if(loadIndex<uint(tileSize.x*tileSize.y)){
  int2 tilePixel=int2(loadIndex%tileSize.x,loadIndex/tileSize.x);
  int2 q=clamp(tileBase+tilePixel,int2(0,0),int2(hw,hh)-1);
  aoTile[tilePixel.y][tilePixel.x]=halfAo.Load(int3(q,0)).r;
  depthTile[tilePixel.y][tilePixel.x]=baseDepth.Load(int3(q,0)).r;
 }
 GroupMemoryBarrierWithGroupSync();
 uint w,h;fullAo.GetDimensions(w,h);if(any(id.xy>=uint2(w,h)))return;
 float center=linearDepth.Load(int3(id.xy,0)).r;if(center>=65500){fullAo[id.xy]=1;return;}
 float2 p=(float2(id.xy)+.5)*settings.inputToOutputScale-.5,inputBase=floor(p),f=frac(p);
 float sum=0,weightSum=0,bestDepthError=1e30,bestAo=1;
 [unroll]for(int y=0;y<=1;++y)[unroll]for(int x=0;x<=1;++x){
  int2 tilePixel=int2(inputBase)+int2(x,y)-tileBase;
  float ao=aoTile[tilePixel.y][tilePixel.x];float d=depthTile[tilePixel.y][tilePixel.x];
  float depthError=abs(d-center);if(depthError<bestDepthError){bestDepthError=depthError;bestAo=ao;}
  float bilinear=(x==0?1-f.x:f.x)*(y==0?1-f.y:f.y);float weight=bilinear*exp(-depthError*settings.depthSigma/max(center,1e-3));sum+=ao*weight;weightSum+=weight;
 }
 fullAo[id.xy]=weightSum>1e-5?saturate(sum/weightSum):bestAo;
}
