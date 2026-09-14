// One 5x5 bilateral denoise dispatch.  A 12x12 groupshared tile gives each
// 8x8 workgroup its two-pixel halo, eliminating repeated global fetches.
[[vk::binding(0,0)]] Sampler2D inputAo;
[[vk::binding(1,0)]] Sampler2D inputGuide;
[[vk::binding(2,0)]] Sampler2D baseDepth;
[[vk::binding(3,0)]] RWTexture2D<float> outputAo;
struct Settings { float depthSigma,normalSigma,edgeSigma; uint padding; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;
groupshared float aoTile[12][12]; groupshared float4 guideTile[12][12]; groupshared float depthTile[12][12];
float3 decodeOct(float2 e){float2 f=e*2-1;float3 n=float3(f,1-abs(f.x)-abs(f.y));if(n.z<0)n.xy=(1-abs(n.yx))*float2(n.x>=0?1:-1,n.y>=0?1:-1);return normalize(n);}
// Four 2-bit, XeGTAO-compatible edge-continuity values packed by gtao_main:
// left, right, top, bottom.
float4 unpackEdges(float payload){uint packed=uint(payload*255.5);return float4((packed>>6)&3,(packed>>4)&3,(packed>>2)&3,packed&3)/3.0;}
float rightLink(int2 p){return unpackEdges(guideTile[p.y][p.x].z).y*unpackEdges(guideTile[p.y][p.x+1].z).x;}
float bottomLink(int2 p){return unpackEdges(guideTile[p.y][p.x].z).w*unpackEdges(guideTile[p.y+1][p.x].z).z;}
// Multiply the symmetric edge links crossed by a 5x5 filter tap.  A tap
// cannot leak across a silhouette even if only one side detected the edge.
float edgePathWeight(int2 origin,int dx,int dy){
 int2 p=origin;float weight=1;
 for(int i=0;i<abs(dx);++i){if(dx>0){weight*=rightLink(p);++p.x;}else{--p.x;weight*=rightLink(p);}}
 for(int i=0;i<abs(dy);++i){if(dy>0){weight*=bottomLink(p);++p.y;}else{--p.y;weight*=bottomLink(p);}}
 return weight;
}
[numthreads(8,8,1)] [shader("compute")]
void main(uint3 groupId:SV_GroupID,uint3 threadId:SV_GroupThreadID,uint3 id:SV_DispatchThreadID){uint w,h;outputAo.GetDimensions(w,h);int2 size=int2(w,h),base=int2(groupId.xy)*8-int2(2,2);for(uint y=threadId.y;y<12;y+=8)for(uint x=threadId.x;x<12;x+=8){int2 p=clamp(base+int2(x,y),int2(0,0),size-1);aoTile[y][x]=inputAo.Load(int3(p,0)).r;guideTile[y][x]=inputGuide.Load(int3(p,0));depthTile[y][x]=baseDepth.Load(int3(p,0)).r;}GroupMemoryBarrierWithGroupSync();if(any(id.xy>=uint2(w,h)))return;int2 q=int2(threadId.xy)+2;float centerDepth=depthTile[q.y][q.x];if(centerDepth>=65500){outputAo[id.xy]=1;return;}float3 n=decodeOct(guideTile[q.y][q.x].xy);float sum=0,weightSum=0;[unroll]for(int y=-2;y<=2;++y)[unroll]for(int x=-2;x<=2;++x){float4 g=guideTile[q.y+y][q.x+x];float depthWeight=rcp(1+abs(depthTile[q.y+y][q.x+x]-centerDepth)*settings.depthSigma/max(centerDepth,1e-3));float normalWeight=pow(saturate(dot(n,decodeOct(g.xy))),settings.normalSigma);float edgeWeight=edgePathWeight(q,x,y);float spatial=exp(-.5*(x*x+y*y));float weight=spatial*depthWeight*normalWeight*edgeWeight;sum+=aoTile[q.y+y][q.x+x]*weight;weightSum+=weight;}outputAo[id.xy]=sum/max(weightSum,1e-5);}
