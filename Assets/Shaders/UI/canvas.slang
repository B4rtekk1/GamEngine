struct ScreenData
{
    float2 inverseExtent;
};

[[vk::push_constant]]
ConstantBuffer<ScreenData> screen;

struct CanvasVertexInput
{
    [[vk::location(0)]] float2 position : POSITION;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float4 color : COLOR0;
    [[vk::location(3)]] float textSample : TEXCOORD1;
};

struct CanvasVertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
    [[vk::location(1)]] float4 color;
    [[vk::location(2)]] float textSample;
};

[shader("vertex")]
CanvasVertexOutput vertexMain(CanvasVertexInput input)
{
    CanvasVertexOutput output;
    float2 ndc = input.position * screen.inverseExtent * 2.0 - 1.0;
    output.position = float4(ndc.x, -ndc.y, 0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    output.textSample = input.textSample;
    return output;
}

[[vk::binding(0, 0)]]
Sampler2D fontAtlas;

struct CanvasFragmentInput
{
    [[vk::location(0)]] float2 uv;
    [[vk::location(1)]] float4 color;
    [[vk::location(2)]] float textSample;
};

struct CanvasFragmentOutput
{
    [[vk::location(0)]] float4 color : SV_Target;
};

[shader("fragment")]
CanvasFragmentOutput fragmentMain(CanvasFragmentInput input)
{
    CanvasFragmentOutput output;
    float coverage = input.textSample > 0.5 ? fontAtlas.Sample(input.uv).r : 1.0;
    output.color = float4(input.color.rgb, input.color.a * coverage);
    return output;
}
