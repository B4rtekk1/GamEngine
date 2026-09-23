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
    uint sw,sh;sourceDepth.GetDimensions(sw,sh);
    float d=sourceDepth.Load(int3(id.xy,0)).r;
    float2 uv=(float2(id.xy)+0.5)/float2(sw,sh);
    destinationDepth[id.xy]=linearRayDepth(d,uv);
}
