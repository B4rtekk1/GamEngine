// One step up the bloom chain: this level's filtered colour plus the
// 3x3 tent-upsampled glow from the next smaller level, so each level adds a
// wider, softer ring and the result sums every radius.
struct V { float4 position : SV_Position; [[vk::location(0)]] float2 uv; };
[shader("vertex")] V vertexMain(uint i : SV_VertexID) { V o; float2 p = float2((i << 1) & 2, i & 2); o.uv = p; o.position = float4(p * 2 - 1, 0, 1); return o; }
[[vk::binding(0, 0)]] Sampler2D levelImage;
[[vk::binding(1, 0)]] Sampler2D smallerImage;
// Texel size of smallerImage.
struct Settings { float inverseWidth, inverseHeight, padding0, padding1; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;

[shader("fragment")] float4 fragmentMain(V input) : SV_Target
{
    const float2 t = float2(settings.inverseWidth, settings.inverseHeight);
    float3 glow = smallerImage.Sample(input.uv).rgb * 4.0;
    glow += (smallerImage.Sample(input.uv + float2(-t.x, 0)).rgb + smallerImage.Sample(input.uv + float2(t.x, 0)).rgb +
             smallerImage.Sample(input.uv + float2(0, -t.y)).rgb + smallerImage.Sample(input.uv + float2(0, t.y)).rgb) * 2.0;
    glow += smallerImage.Sample(input.uv + float2(-t.x, -t.y)).rgb + smallerImage.Sample(input.uv + float2(t.x, -t.y)).rgb +
            smallerImage.Sample(input.uv + float2(-t.x, t.y)).rgb + smallerImage.Sample(input.uv + float2(t.x, t.y)).rgb;
    return float4(levelImage.Sample(input.uv).rgb + glow / 16.0, 1);
}
