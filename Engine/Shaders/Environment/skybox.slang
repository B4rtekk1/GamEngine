import Common.SceneData;

#ifndef SKY_OUTPUT_VELOCITY
#define SKY_OUTPUT_VELOCITY 1
#endif

[[vk::binding(0, 0)]]
ConstantBuffer<RendererFrameData> camera;

struct SkyboxVertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
};

struct SkyboxVertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 direction;
#if SKY_OUTPUT_VELOCITY
    [[vk::location(1)]] noperspective float2 currentNdc;
    [[vk::location(2)]] float4 previousClip;
#endif
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
#if SKY_OUTPUT_VELOCITY
    float4x4 previousView = camera.previousView;
    previousView[3][0] = 0.0;
    previousView[3][1] = 0.0;
    previousView[3][2] = 0.0;
    previousView[3][3] = 1.0;
    output.currentNdc = output.position.xy / output.position.w;
    float4 previous = mul(mul(float4(input.position, 1.0), previousView), camera.previousProjection);
    output.previousClip = previous.xyww;
#endif
    return output;
}

[[vk::binding(1, 0)]]
SamplerCube environmentMap;

struct SkyboxFragmentInput
{
    [[vk::location(0)]] float3 direction;
#if SKY_OUTPUT_VELOCITY
    [[vk::location(1)]] noperspective float2 currentNdc;
    [[vk::location(2)]] float4 previousClip;
#endif
};

struct SkyboxFragmentOutput
{
    [[vk::location(0)]] float4 color : SV_Target;
#if SKY_OUTPUT_VELOCITY
    [[vk::location(1)]] float2 velocity : SV_Target1;
#endif
};

[shader("fragment")]
SkyboxFragmentOutput fragmentMain(SkyboxFragmentInput input)
{
    SkyboxFragmentOutput output;
    output.color = environmentMap.Sample(normalize(input.direction));
#if SKY_OUTPUT_VELOCITY
    output.velocity = (input.currentNdc - input.previousClip.xy / input.previousClip.w) * 0.5;
#endif
    return output;
}
