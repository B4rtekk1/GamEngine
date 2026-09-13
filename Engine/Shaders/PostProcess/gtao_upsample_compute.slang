[[vk::binding(0,0)]] Sampler2D halfAo;
[[vk::binding(1,0)]] Sampler2D halfGuide;
[[vk::binding(2,0)]] Sampler2D fullDepth;
[[vk::binding(3,0)]] RWTexture2D<float> fullAo;
struct Settings { float depthSigma,padding0,padding1,padding2; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;
[numthreads(8,8,1)] [shader("compute")]
void main(uint3 id:SV_DispatchThreadID){uint w,h;fullAo.GetDimensions(w,h);if(any(id.xy>=uint2(w,h)))return;float center=fullDepth.Load(int3(id.xy,0)).r;if(center>=65500){fullAo[id.xy]=1;return;}uint hw,hh;halfAo.GetDimensions(hw,hh);float2 p=(float2(id.xy)+.5)*.5-.5,base=floor(p),f=frac(p);float sum=0,weightSum=0;[unroll]for(int y=0;y<=1;++y)[unroll]for(int x=0;x<=1;++x){int2 q=clamp(int2(base)+int2(x,y),int2(0,0),int2(hw,hh)-1);float ao=halfAo.Load(int3(q,0)).r;float d=halfGuide.Load(int3(q,0)).r;float bilinear=(x==0?1-f.x:f.x)*(y==0?1-f.y:f.y);float weight=bilinear*exp(-abs(d-center)*settings.depthSigma/max(center,1e-3));sum+=ao*weight;weightSum+=weight;}fullAo[id.xy]=saturate(sum/max(weightSum,1e-5));}
