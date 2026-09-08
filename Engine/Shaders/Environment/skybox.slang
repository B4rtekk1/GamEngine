struct CameraData
{
    float4x4 view;
    float4x4 projection;
};

[[vk::binding(0, 0)]]
ConstantBuffer<CameraData> camera;

struct SkyboxVertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
};

struct SkyboxVertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 direction;
};

[shader("vertex")]
SkyboxVertexOutput vertexMain(SkyboxVertexInput input)
{
    SkyboxVertexOutput output;
    output.direction = input.position;

    float4x4 view = camera.view;
    view[3][0] = 0.0;
    view[3][1] = 0.0;
    view[3][2] = 0.0;
    view[3][3] = 1.0;
    float4 position = mul(mul(float4(input.position, 1.0), view), camera.projection);
    output.position = position.xyww;
    return output;
}

[[vk::binding(1, 0)]]
SamplerCube environmentMap;

struct SkyboxFragmentInput
{
    [[vk::location(0)]] float3 direction;
};

struct SkyboxFragmentOutput
{
    [[vk::location(0)]] float4 color : SV_Target;
};

[shader("fragment")]
SkyboxFragmentOutput fragmentMain(SkyboxFragmentInput input)
{
    SkyboxFragmentOutput output;
    output.color = environmentMap.Sample(normalize(input.direction));
    return output;
}
