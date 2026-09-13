// One 5x5 bilateral denoise dispatch.  A 12x12 groupshared tile gives each
// 8x8 workgroup its two-pixel halo, eliminating repeated global fetches.
[[vk::binding(0,0)]] Sampler2D inputAo;
[[vk::binding(1,0)]] Sampler2D inputGuide;
[[vk::binding(2,0)]] RWTexture2D<float> outputAo;
struct Settings { float depthSigma,normalSigma,edgeSigma,padding; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;
groupshared float aoTile[12][12]; groupshared float4 guideTile[12][12];
float3 decodeOct(float2 e){float2 f=e*2-1;float3 n=float3(f,1-abs(f.x)-abs(f.y));if(n.z<0)n.xy=(1-abs(n.yx))*float2(n.x>=0?1:-1,n.y>=0?1:-1);return normalize(n);}
[numthreads(8,8,1)] [shader("compute")]
void main(uint3 groupId:SV_GroupID,uint3 threadId:SV_GroupThreadID,uint3 id:SV_DispatchThreadID){uint w,h;outputAo.GetDimensions(w,h);int2 size=int2(w,h),base=int2(groupId.xy)*8-int2(2,2);for(uint y=threadId.y;y<12;y+=8)for(uint x=threadId.x;x<12;x+=8){int2 p=clamp(base+int2(x,y),int2(0,0),size-1);aoTile[y][x]=inputAo.Load(int3(p,0)).r;guideTile[y][x]=inputGuide.Load(int3(p,0));}GroupMemoryBarrierWithGroupSync();if(any(id.xy>=uint2(w,h)))return;int2 q=int2(threadId.xy)+2;float4 center=guideTile[q.y][q.x];if(center.x>=65500){outputAo[id.xy]=1;return;}float3 n=decodeOct(center.yz);float sum=0,weightSum=0;[unroll]for(int y=-2;y<=2;++y)[unroll]for(int x=-2;x<=2;++x){float4 g=guideTile[q.y+y][q.x+x];float depthWeight=rcp(1+abs(g.x-center.x)*settings.depthSigma/max(center.x,1e-3));float normalWeight=pow(saturate(dot(n,decodeOct(g.yz))),settings.normalSigma);float edgeWeight=rcp(1+max(g.w,center.w)*settings.edgeSigma);float spatial=exp(-.5*(x*x+y*y));float weight=spatial*depthWeight*normalWeight*edgeWeight;sum+=aoTile[q.y+y][q.x+x]*weight;weightSum+=weight;}outputAo[id.xy]=sum/max(weightSum,1e-5);}
