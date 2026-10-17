// XeGTAO's edge-continuity denoiser. An 18x10 shared tile covers each 16x8
// output workgroup plus the one-pixel halo required by the 3x3 filter.
[[vk::binding(0,0)]] Sampler2D inputAo;
[[vk::binding(1,0)]] Sampler2D inputGuide;
[[vk::binding(2,0)]] RWTexture2D<float> outputAo;
struct Settings { float blurAmount; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;
groupshared float aoTile[10][18];
groupshared uint edgeTile[10][18];

float4 unpackEdges(uint packed){return saturate(float4((packed>>6)&3,(packed>>4)&3,(packed>>2)&3,packed&3)/3.0);}
void addSample(float value,float weight,inout float sum,inout float weightSum){sum+=weight*value;weightSum+=weight;}

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
 float4 center=unpackEdges(edgeTile[c.y][c.x]);
 float4 left=unpackEdges(edgeTile[c.y][c.x-1u]);
 float4 right=unpackEdges(edgeTile[c.y][c.x+1u]);
 float4 top=unpackEdges(edgeTile[c.y-1u][c.x]);
 float4 bottom=unpackEdges(edgeTile[c.y+1u][c.x]);
 // Reject samples across depth discontinuities on both sides of an edge.
 center*=float4(left.y,right.x,top.w,bottom.z);
 // Allow a small amount of leakage at corners; this prevents isolated
 // pixels from becoming unnecessarily dark after edge rejection.
 float edginess=saturate(1.5-dot(center,1.0.xxxx))/1.5*.5;
 center=saturate(center+edginess);
 const float diagonalWeight=.85*.5;
 float weightTL=diagonalWeight*(center.x*left.z+center.z*top.x);
 float weightTR=diagonalWeight*(center.z*top.y+center.y*right.z);
 float weightBL=diagonalWeight*(center.w*bottom.x+center.x*left.w);
 float weightBR=diagonalWeight*(center.y*right.w+center.w*bottom.y);
 float weightSum=settings.blurAmount,sum=aoTile[c.y][c.x]*weightSum;
 addSample(aoTile[c.y][c.x-1u],center.x,sum,weightSum);addSample(aoTile[c.y][c.x+1u],center.y,sum,weightSum);
 addSample(aoTile[c.y-1u][c.x],center.z,sum,weightSum);addSample(aoTile[c.y+1u][c.x],center.w,sum,weightSum);
 addSample(aoTile[c.y-1u][c.x-1u],weightTL,sum,weightSum);addSample(aoTile[c.y-1u][c.x+1u],weightTR,sum,weightSum);
 addSample(aoTile[c.y+1u][c.x-1u],weightBL,sum,weightSum);addSample(aoTile[c.y+1u][c.x+1u],weightBR,sum,weightSum);
 outputAo[uint2(outputX,id.y)]=sum/max(weightSum,1e-5);
 }
}
