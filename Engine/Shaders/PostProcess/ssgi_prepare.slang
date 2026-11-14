// Packs the SSGI trace inputs at trace resolution: linear view depth and the
// octahedral view normal in one 8-byte texel. Every trace step then makes a
// single fetch from a texture a quarter the size of the depth and normal
// targets, instead of two cache-hostile full-resolution reads.
import PostProcess.SsgiCommon;

[[vk::binding(0,0)]] Sampler2D sceneDepth;
[[vk::binding(1,0)]] Sampler2D viewNormalTexture;
[[vk::binding(2,0), vk::image_format("rg32ui")]] RWTexture2D<uint2> geometry;
struct Settings { float4 depthToView; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;

[numthreads(8,8,1)] [shader("compute")]
void main(uint3 id:SV_DispatchThreadID){
    uint w, h, fullW, fullH;
    geometry.GetDimensions(w, h);
    if (any(id.xy >= uint2(w, h))) return;
    sceneDepth.GetDimensions(fullW, fullH);
    // The same full-resolution texel the trace and resolve treat as the
    // half-resolution pixel's representative.
    const int2 source = min(int2((float2(id.xy) + 0.5) / float2(w, h) * float2(fullW, fullH)), int2(fullW, fullH) - 1);
    const float depth = ssgiLinearizeDepth(sceneDepth.Load(int3(source, 0)).r, settings.depthToView);
    geometry[id.xy] = uint2(asuint(depth), ssgiPackOct(viewNormalTexture.Load(int3(source, 0)).xy));
}
