// Final denoise pass fused with the bilateral upsample for reduced-resolution
// GTAO. Each group denoises only the source texels its bilinear taps touch,
// so the filtered AO never round-trips through memory at 8 bits.
import PostProcess.GtaoDenoiseCommon;

[[vk::binding(0,0)]] Sampler2D inputAo;
[[vk::binding(1,0)]] Sampler2D inputGuide;
[[vk::binding(2,0)]] Sampler2D linearDepth;
[[vk::binding(3,0)]] Sampler2D baseDepth;
[[vk::binding(4,0)]] RWTexture2D<float> fullAo;
// The denoised source is still published for the Filtered debug view.
[[vk::binding(5,0)]] RWTexture2D<float> filteredAo;
// `inputToOutputScale` is input resolution / output resolution.
struct Settings { float2 inputToOutputScale; float depthSigma, blurAmount; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;
// An 8x8 output group spans at most 9x9 source texels for any scale below
// one; the 3x3 denoise needs one more texel on each side.
groupshared float rawTile[11][11];
groupshared uint edgeTile[11][11];
groupshared float aoTile[9][9];
groupshared float depthTile[9][9];

[numthreads(8,8,1)] [shader("compute")]
void main(uint3 groupId:SV_GroupID,uint3 threadId:SV_GroupThreadID,uint3 id:SV_DispatchThreadID){
 uint hw,hh;inputAo.GetDimensions(hw,hh);int2 maxPixel=int2(hw,hh)-1;
 float2 scale=settings.inputToOutputScale;
 int2 tileBase=int2(floor((float2(groupId.xy*8)+.5)*scale-.5));
 int2 tileSize=min(int2(floor((float2(groupId.xy*8)+7.5)*scale-.5))+2-tileBase,int2(9,9));
 int2 rawBase=tileBase-1,rawSize=tileSize+2;
 uint lane=threadId.y*8+threadId.x;
 for(uint i=lane;i<uint(rawSize.x*rawSize.y);i+=64){
  int2 t=int2(i%uint(rawSize.x),i/uint(rawSize.x));
  int2 q=clamp(rawBase+t,int2(0,0),maxPixel);
  rawTile[t.y][t.x]=inputAo.Load(int3(q,0)).r;
  edgeTile[t.y][t.x]=uint(inputGuide.Load(int3(q,0)).r*255.5);
 }
 GroupMemoryBarrierWithGroupSync();
 for(uint i=lane;i<uint(tileSize.x*tileSize.y);i+=64){
  int2 t=int2(i%uint(tileSize.x),i/uint(tileSize.x));
  int2 c=tileBase+t,q=clamp(c,int2(0,0),maxPixel);
  // Match the standalone pass exactly: clamp the centre, then clamp each tap.
  int2 lo=max(q-1,int2(0,0))-rawBase,hi=min(q+1,maxPixel)-rawBase,m=q-rawBase;
  float ao[9]={rawTile[lo.y][lo.x],rawTile[lo.y][m.x],rawTile[lo.y][hi.x],
               rawTile[m.y][lo.x],rawTile[m.y][m.x],rawTile[m.y][hi.x],
               rawTile[hi.y][lo.x],rawTile[hi.y][m.x],rawTile[hi.y][hi.x]};
  float denoised=gtaoDenoise(ao,edgeTile[m.y][m.x],edgeTile[m.y][lo.x],edgeTile[m.y][hi.x],
                             edgeTile[lo.y][m.x],edgeTile[hi.y][m.x],settings.blurAmount);
  aoTile[t.y][t.x]=denoised;
  depthTile[t.y][t.x]=baseDepth.Load(int3(q,0)).r;
  // Neighbouring tiles overlap; the group whose output pixels a texel's
  // centre maps into owns its debug write.
  uint2 owner=uint2(floor((float2(c)+.5)/scale))/8u;
  if(all(c==q)&&all(owner==groupId.xy))filteredAo[c]=denoised;
 }
 GroupMemoryBarrierWithGroupSync();
 uint w,h;fullAo.GetDimensions(w,h);if(any(id.xy>=uint2(w,h)))return;
 float center=linearDepth.Load(int3(id.xy,0)).r;if(center>=65500){fullAo[id.xy]=1;return;}
 float2 p=(float2(id.xy)+.5)*scale-.5,inputBase=floor(p),f=frac(p);
 float sum=0,weightSum=0,bestDepthError=1e30,bestAo=1;
 [unroll]for(int y=0;y<=1;++y)[unroll]for(int x=0;x<=1;++x){
  int2 tilePixel=int2(inputBase)+int2(x,y)-tileBase;
  float ao=aoTile[tilePixel.y][tilePixel.x];float d=depthTile[tilePixel.y][tilePixel.x];
  float depthError=abs(d-center);if(depthError<bestDepthError){bestDepthError=depthError;bestAo=ao;}
  float bilinear=(x==0?1-f.x:f.x)*(y==0?1-f.y:f.y);float weight=bilinear*exp(-depthError*settings.depthSigma/max(center,1e-3));sum+=ao*weight;weightSum+=weight;
 }
 fullAo[id.xy]=weightSum>1e-5?saturate(sum/weightSum):bestAo;
}
