// One step down the bloom chain: a 4-tap box over a 2x2 texel footprint of
// the next larger level. The first step also applies the soft threshold and
// Karis-weights its taps by 1 / (1 + luma), so a single very bright texel (a
// lamp bulb) cannot flicker the whole glow as TAA jitter moves it.
struct V { float4 position : SV_Position; [[vk::location(0)]] float2 uv; };
[shader("vertex")] V vertexMain(uint i : SV_VertexID) { V o; float2 p = float2((i << 1) & 2, i & 2); o.uv = p; o.position = float4(p * 2 - 1, 0, 1); return o; }
[[vk::binding(0, 0)]] Sampler2D sourceImage;
// threshold < 0: a plain filter step without thresholding.
struct Settings { float threshold, knee, inverseWidth, inverseHeight; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;

float karisWeight(float3 c) { return 1.0 / (1.0 + max(c.r, max(c.g, c.b))); }

[shader("fragment")] float4 fragmentMain(V input) : SV_Target
{
    const float2 t = float2(settings.inverseWidth, settings.inverseHeight);
    const float3 a = sourceImage.Sample(input.uv + t * float2(-1, -1)).rgb;
    const float3 b = sourceImage.Sample(input.uv + t * float2(1, -1)).rgb;
    const float3 c = sourceImage.Sample(input.uv + t * float2(-1, 1)).rgb;
    const float3 d = sourceImage.Sample(input.uv + t * float2(1, 1)).rgb;
    if (settings.threshold < 0.0)
        return float4((a + b + c + d) * 0.25, 1);
    const float wa = karisWeight(a), wb = karisWeight(b), wc = karisWeight(c), wd = karisWeight(d);
    const float3 color = (a * wa + b * wb + c * wc + d * wd) / (wa + wb + wc + wd);
    const float brightness = max(color.r, max(color.g, color.b));
    float soft = clamp(brightness - settings.threshold + settings.knee, 0, 2 * settings.knee);
    soft = soft * soft / max(4 * settings.knee, 1e-4);
    return float4(color * max(brightness - settings.threshold, soft) / max(brightness, 1e-4), 1);
}
