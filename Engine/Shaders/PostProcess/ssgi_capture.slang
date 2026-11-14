// Stores this frame's lit opaque colour at SSGI resolution. The next frame's
// trace reprojects into it, so every frame adds one more diffuse bounce.
import PostProcess.SsgiCommon;

[[vk::binding(0,0)]] Sampler2D sceneColor;
[[vk::binding(1,0)]] RWTexture2D<float4> radiance;
// Specular highlights and the sun disc are far brighter than any diffuse
// emitter; clamping them keeps single taps from turning into fireflies.
struct Settings { float maxLuminance; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;

[numthreads(8,8,1)] [shader("compute")]
void main(uint3 id:SV_DispatchThreadID){
    uint w, h;
    radiance.GetDimensions(w, h);
    if (any(id.xy >= uint2(w, h))) return;
    // Exactly between the four source texels, so bilinear filtering is a 2x2 box.
    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    float3 color = sceneColor.SampleLevel(uv, 0.0).rgb;
    if (any(isnan(color)) || any(isinf(color))) color = 0.0;
    color = max(color, 0.0);
    float luminance = ssgiLuminance(color);
    if (luminance > settings.maxLuminance) color *= settings.maxLuminance / luminance;
    radiance[id.xy] = float4(color, 1.0);
}
