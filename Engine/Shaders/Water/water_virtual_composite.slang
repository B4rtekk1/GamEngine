import Common.SceneData;

[[vk::binding(1,0)]] ConstantBuffer<RendererFrameData> frame;

[[vk::binding(0,1)]] Sampler2D opaqueColor;
[[vk::binding(1,1)]] Sampler2D waterLighting;
[[vk::binding(2,1)]] Sampler2D waterMeta;
[[vk::binding(3,1)]] Sampler2D opaqueDepth;
struct UnderwaterConfig {
    float shallowR, shallowG, shallowB, immersion;
    float deepR, deepG, deepB, maxDepth;
    float absorptionR, absorptionG, absorptionB, time;
    float scatteringR, scatteringG, scatteringB, surfaceHeight;
    uint active, causticsEnabled, materialIndex, padding;
};
[[vk::binding(4,1)]] ConstantBuffer<UnderwaterConfig> underwater;
[[vk::binding(5,1)]] Sampler2D waterSurface;

struct VSOut { float4 position:SV_Position; [[vk::location(0)]] float2 uv; };
[shader("vertex")]
VSOut vertexMain(uint id:SV_VertexID) {
    float2 p=id==0?float2(-1,-1):id==1?float2(3,-1):float2(-1,3);
    VSOut o;o.position=float4(p,0,1);o.uv=p*0.5+0.5;return o;
}

float viewDistanceFromDepth(float depth,float2 uv) {
    if(depth>=0.99999) return underwater.maxDepth;
    float4 view=mul(float4(uv*2.0-1.0,depth,1.0),frame.inverseProjection);
    return length(view.xyz/max(abs(view.w),1.0e-5));
}

[shader("fragment")]
float4 fragmentMain(VSOut i):SV_Target {
    uint q=asuint(waterMeta.SampleLevel(i.uv,0).y);
    const bool waterPixel=(q&0x80000000u)!=0;
    float3 color=waterPixel?waterLighting.SampleLevel(i.uv,0).rgb:opaqueColor.SampleLevel(i.uv,0).rgb;
    if(underwater.active!=0u) {
        // Water pixels use the actual displaced surface depth; ordinary pixels
        // use opaque receiver depth. This turns attenuation into a view-space
        // distance instead of treating non-linear device Z as a metric length.
        float depth=waterPixel?waterSurface.SampleLevel(i.uv,0).z:opaqueDepth.SampleLevel(i.uv,0).r;
        float receiverDistance=viewDistanceFromDepth(depth,i.uv);
        float distanceThroughWater=clamp(max(underwater.immersion,receiverDistance),0.0,underwater.maxDepth*4.0);
        float3 sigmaA=max(float3(underwater.absorptionR,underwater.absorptionG,underwater.absorptionB),0.0);
        float3 sigmaS=max(float3(underwater.scatteringR,underwater.scatteringG,underwater.scatteringB),0.0);
        float3 sigmaT=max(sigmaA+sigmaS,float3(1.0e-4));
        float3 trans=exp(-sigmaT*distanceThroughWater);
        float deepFactor=saturate(distanceThroughWater/max(underwater.maxDepth,1e-3));
        float3 tint=lerp(float3(underwater.shallowR,underwater.shallowG,underwater.shallowB),
                         float3(underwater.deepR,underwater.deepG,underwater.deepB),deepFactor);
        float3 inScatter=(sigmaS/sigmaT)*(1.0-trans)*tint;
        color=color*trans+inScatter;

        // Lightweight receiver-space modulation. Surface water uses the full
        // Jacobian-based caustic term in water_virtual_shade; this final term
        // keeps submerged opaque receivers coherent when the camera is below
        // the surface without allocating another full-resolution render target.
        if(underwater.causticsEnabled!=0u) {
            float caustic=(0.5+0.5*sin((i.uv.x*37.0+i.uv.y*29.0)+underwater.time*2.2))*
                           exp(-underwater.immersion*0.35)*exp(-distanceThroughWater*0.045);
            color+=tint*caustic*0.025;
        }
    }
    return float4(color,1.0);
}
