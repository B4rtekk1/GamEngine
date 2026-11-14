// Seeds the water-only depth target with opaque scene depth. Water then depth
// tests against opaque geometry and against itself (nearest crest wins)
// without ever writing into the scene depth buffer that shading samples.
[[vk::binding(0,0)]] Sampler2D opaqueDepth;

struct VSOut { float4 position:SV_Position; };

[shader("vertex")]
VSOut vertexMain(uint id:SV_VertexID){
    float2 p=id==0?float2(-1,-1):id==1?float2(3,-1):float2(-1,3);
    VSOut o;o.position=float4(p,0,1);return o;
}

struct PSOut{
    [[vk::location(0)]] float4 surface:SV_Target0;
    [[vk::location(1)]] float2 meta:SV_Target1;
    [[vk::location(2)]] float2 velocity:SV_Target2;
    float depth:SV_Depth;
};

[shader("fragment")]
PSOut fragmentMain(VSOut i){
    // Color writes are masked off by the pipeline; the attachments keep their clear values.
    PSOut o=(PSOut)0;
    o.depth=opaqueDepth.Load(int3(int2(i.position.xy),0)).r;
    return o;
}
