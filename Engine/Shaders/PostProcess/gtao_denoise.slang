// XeGTAO's edge-continuity denoiser.  It deliberately does not use depth or
// normal similarity: local edge continuity alone determines which of the
// eight neighbouring AO values may contribute.
[[vk::binding(0,0)]] Sampler2D inputAo;
[[vk::binding(1,0)]] Sampler2D inputGuide;
[[vk::binding(2,0)]] RWTexture2D<float> outputAo;
struct Settings { float blurAmount; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;
// Four 2-bit, XeGTAO-compatible edge-continuity values packed by gtao_main:
// left, right, top, bottom.
float4 unpackEdges(float payload){uint packed=uint(payload*255.5);return saturate(float4((packed>>6)&3,(packed>>4)&3,(packed>>2)&3,packed&3)/3.0);}
float4 edgesAt(int2 p,int2 size){return unpackEdges(inputGuide.Load(int3(clamp(p,int2(0,0),size-1),0)).z);}
void addSample(float value,float weight,inout float sum,inout float weightSum){sum+=weight*value;weightSum+=weight;}
float aoAt(int2 p,int2 size){return inputAo.Load(int3(clamp(p,int2(0,0),size-1),0)).r;}
[numthreads(8,8,1)] [shader("compute")]
void main(uint3 id:SV_DispatchThreadID){
 uint w,h;outputAo.GetDimensions(w,h);if(any(id.xy>=uint2(w,h)))return;int2 size=int2(w,h),p=int2(id.xy);
 float4 center=edgesAt(p,size),left=edgesAt(p+int2(-1,0),size),right=edgesAt(p+int2(1,0),size),top=edgesAt(p+int2(0,-1),size),bottom=edgesAt(p+int2(0,1),size);
 // Make links symmetric. Do not reopen rejected links: doing so lets AO
 // cross silhouette discontinuities and produces visible halos.
 center*=float4(left.y,right.x,top.w,bottom.z);
 center=saturate(center);
 const float diagonalWeight=.85*.5;
 float weightTL=diagonalWeight*(center.x*left.z+center.z*top.x);
 float weightTR=diagonalWeight*(center.z*top.y+center.y*right.z);
 float weightBL=diagonalWeight*(center.w*bottom.x+center.x*left.w);
 float weightBR=diagonalWeight*(center.y*right.w+center.w*bottom.y);
 float weightSum=settings.blurAmount,sum=aoAt(p,size)*weightSum;
 addSample(aoAt(p+int2(-1,0),size),center.x,sum,weightSum);addSample(aoAt(p+int2(1,0),size),center.y,sum,weightSum);
 addSample(aoAt(p+int2(0,-1),size),center.z,sum,weightSum);addSample(aoAt(p+int2(0,1),size),center.w,sum,weightSum);
 addSample(aoAt(p+int2(-1,-1),size),weightTL,sum,weightSum);addSample(aoAt(p+int2(1,-1),size),weightTR,sum,weightSum);
 addSample(aoAt(p+int2(-1,1),size),weightBL,sum,weightSum);addSample(aoAt(p+int2(1,1),size),weightBR,sum,weightSum);
 outputAo[p]=sum/max(weightSum,1e-5);
}
