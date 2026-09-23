// XeGTAO's edge-continuity denoiser. A 10x10 shared tile covers each 8x8
// output workgroup plus the one-pixel halo required by the 3x3 filter.
[[vk::binding(0,0)]] Sampler2D inputAo;
[[vk::binding(1,0)]] Sampler2D inputGuide;
[[vk::binding(2,0)]] RWTexture2D<float> outputAo;
struct Settings { float blurAmount; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;
groupshared float aoTile[10][10];
groupshared uint edgeTile[10][10];

float4 unpackEdges(uint packed){return saturate(float4((packed>>6)&3,(packed>>4)&3,(packed>>2)&3,packed&3)/3.0);}
void addSample(float value,float weight,inout float sum,inout float weightSum){sum+=weight*value;weightSum+=weight;}

[numthreads(8,8,1)] [shader("compute")]
void main(uint3 groupId:SV_GroupID,uint3 threadId:SV_GroupThreadID,uint3 id:SV_DispatchThreadID){
 uint w,h;outputAo.GetDimensions(w,h);int2 size=int2(w,h);
 uint lane=threadId.y*8u+threadId.x;
 for(uint i=lane;i<100u;i+=64u){
  uint2 tileCoord=uint2(i%10u,i/10u);
  int2 q=clamp(int2(groupId.xy*8u+tileCoord)-1,int2(0,0),size-1);
  aoTile[tileCoord.y][tileCoord.x]=inputAo.Load(int3(q,0)).r;
  edgeTile[tileCoord.y][tileCoord.x]=uint(inputGuide.Load(int3(q,0)).r*255.5);
 }
 GroupMemoryBarrierWithGroupSync();
 if(any(id.xy>=uint2(w,h)))return;

 uint2 c=threadId.xy+1u;
 float4 center=unpackEdges(edgeTile[c.y][c.x]);
 float4 left=unpackEdges(edgeTile[c.y][c.x-1u]);
 float4 right=unpackEdges(edgeTile[c.y][c.x+1u]);
 float4 top=unpackEdges(edgeTile[c.y-1u][c.x]);
 float4 bottom=unpackEdges(edgeTile[c.y+1u][c.x]);
 // Reject samples across depth discontinuities on both sides of an edge.
 center*=float4(left.y,right.x,top.w,bottom.z);
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
 outputAo[id.xy]=sum/max(weightSum,1e-5);
}
