import Common.SceneData;
[[vk::binding(1,0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(0,1)]] Sampler2D sourceDepth;
[[vk::binding(1,1)]] RWTexture2D<float> destinationDepth;

float linearRayDepth(float depth,float2 uv){
    float4 v=mul(float4(uv*2.0-1.0,depth,1.0),frame.inverseProjection);
    float3 p=v.xyz/max(abs(v.w),1e-6);
    return max(-p.z,0.0);
}

[numthreads(8,8,1)] [shader("compute")]
void main(uint3 id:SV_DispatchThreadID){
    uint dw,dh;destinationDepth.GetDimensions(dw,dh);if(id.x>=dw||id.y>=dh)return;
    uint sw,sh;sourceDepth.GetDimensions(sw,sh);uint2 base=id.xy*2u;
    uint2 p[4]={min(base,uint2(sw-1,sh-1)),min(base+uint2(1,0),uint2(sw-1,sh-1)),min(base+uint2(0,1),uint2(sw-1,sh-1)),min(base+uint2(1,1),uint2(sw-1,sh-1))};
    float result=1e30;
    [unroll]for(uint i=0;i<4;++i){float d=sourceDepth.Load(int3(p[i],0)).r;float2 uv=(float2(p[i])+0.5)/float2(sw,sh);result=min(result,linearRayDepth(d,uv));}
    destinationDepth[id.xy]=result;
}
