// XeGTAO's edge-continuity denoiser. An 18x10 shared tile covers each 16x8
// output workgroup plus the one-pixel halo required by the 3x3 filter.
import PostProcess.GtaoDenoiseCommon;

[[vk::binding(0,0)]] Sampler2D inputAo;
[[vk::binding(1,0)]] Sampler2D inputGuide;
[[vk::binding(2,0)]] RWTexture2D<float> outputAo;
struct Settings { float blurAmount; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;
groupshared float aoTile[10][18];
groupshared uint edgeTile[10][18];

[numthreads(8,8,1)] [shader("compute")]
void main(uint3 groupId:SV_GroupID,uint3 threadId:SV_GroupThreadID,uint3 id:SV_DispatchThreadID){
 uint w,h;outputAo.GetDimensions(w,h);int2 size=int2(w,h);
 uint lane=threadId.y*8u+threadId.x;
 for(uint i=lane;i<180u;i+=64u){
  uint2 tileCoord=uint2(i%18u,i/18u);
  int2 q=clamp(int2(groupId.x*16u+tileCoord.x,groupId.y*8u+tileCoord.y)-1,int2(0,0),size-1);
  aoTile[tileCoord.y][tileCoord.x]=inputAo.Load(int3(q,0)).r;
  edgeTile[tileCoord.y][tileCoord.x]=uint(inputGuide.Load(int3(q,0)).r*255.5);
 }
 GroupMemoryBarrierWithGroupSync();
 if(any(id.xy>=uint2(w,h)))return;

 for(uint horizontal=0u;horizontal<2u;++horizontal){
 uint outputX=id.x*2u+horizontal;
 if(outputX>=w)continue;
 uint2 c=uint2(threadId.x*2u+horizontal+1u,threadId.y+1u);
 float ao[9]={aoTile[c.y-1u][c.x-1u],aoTile[c.y-1u][c.x],aoTile[c.y-1u][c.x+1u],
              aoTile[c.y][c.x-1u],aoTile[c.y][c.x],aoTile[c.y][c.x+1u],
              aoTile[c.y+1u][c.x-1u],aoTile[c.y+1u][c.x],aoTile[c.y+1u][c.x+1u]};
 outputAo[uint2(outputX,id.y)]=gtaoDenoise(ao,edgeTile[c.y][c.x],edgeTile[c.y][c.x-1u],edgeTile[c.y][c.x+1u],
                                           edgeTile[c.y-1u][c.x],edgeTile[c.y+1u][c.x],settings.blurAmount);
 }
}
