struct TonemapVertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
};

[shader("vertex")]
TonemapVertexOutput vertexMain(uint vertexIndex : SV_VertexID)
{
    TonemapVertexOutput output;
    float2 position = float2((vertexIndex << 1) & 2, vertexIndex & 2);
    output.uv = position;
    output.position = float4(position * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

[[vk::binding(0, 0)]]
Sampler2D hdrImage;

struct TonemapSettings
{
    float exposure;
    float applyGamma;
};

[[vk::push_constant]]
ConstantBuffer<TonemapSettings> settings;

struct TonemapFragmentInput
{
    [[vk::location(0)]] float2 uv;
};

struct TonemapFragmentOutput
{
    [[vk::location(0)]] float4 color : SV_Target;
};

float3 acesFilm(float3 color)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) /
                 (color * (c * color + d) + e), 0.0, 1.0);
}

[shader("fragment")]
TonemapFragmentOutput fragmentMain(TonemapFragmentInput input)
{
    TonemapFragmentOutput output;
    float3 color = hdrImage.Sample(input.uv).rgb * exp2(settings.exposure);
    color = acesFilm(color);
    if (settings.applyGamma > 0.5)
        color = pow(color, float3(1.0 / 2.2));
    output.color = float4(color, 1.0);
    return output;
}
