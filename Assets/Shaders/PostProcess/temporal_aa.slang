struct VertexOutput { float4 position : SV_Position; [[vk::location(0)]] float2 uv; };
[shader("vertex")]
VertexOutput vertexMain(uint index : SV_VertexID) {
    VertexOutput output; float2 p = float2((index << 1) & 2, index & 2);
    output.uv = p; output.position = float4(p * 2.0 - 1.0, 0.0, 1.0); return output;
}
[[vk::binding(0, 0)]] Sampler2D currentFrame;
[[vk::binding(1, 0)]] Sampler2D historyFrame;
struct Settings { float currentJitterX, currentJitterY, previousJitterX, previousJitterY, historyWeight, inverseWidth, inverseHeight, padding; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;
struct FragmentInput { [[vk::location(0)]] float2 uv; };
struct FragmentOutput { [[vk::location(0)]] float4 color : SV_Target; };
[shader("fragment")]
FragmentOutput fragmentMain(FragmentInput input) {
    FragmentOutput output; float3 current = currentFrame.Sample(input.uv).rgb;
    float2 historyUv = input.uv + 0.5 * float2(settings.currentJitterX - settings.previousJitterX, settings.currentJitterY - settings.previousJitterY);
    float3 history = historyFrame.Sample(historyUv).rgb;
    float3 lo = current; float3 hi = current;
    float2 texel = float2(settings.inverseWidth, settings.inverseHeight);
    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) { float3 sample = currentFrame.Sample(input.uv + float2(x, y) * texel).rgb; lo = min(lo, sample); hi = max(hi, sample); }
    history = clamp(history, lo, hi);
    output.color = float4(lerp(current, history, settings.historyWeight), 1.0); return output;
}
