import Particles.ParticleTypes;

[[vk::binding(0, 0)]] StructuredBuffer<Particle> particles;
[[vk::binding(2, 0)]] StructuredBuffer<uint> activeIndices;
[[vk::binding(1, 0)]] ConstantBuffer<ParticleFrameData> frame;

struct ParticleVertexInput
{
    [[vk::location(0)]] float2 position : POSITION;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
};

struct ParticleVertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
    [[vk::location(1)]] float4 color;
};

[shader("vertex")]
ParticleVertexOutput vertexMain(ParticleVertexInput input, uint instanceIndex : SV_InstanceID)
{
    Particle particle = particles[activeIndices[instanceIndex]];
    float3 world = particle.positionLife.xyz +
        (frame.cameraRight * input.position.x + frame.cameraUp * input.position.y) *
        particle.velocitySize.w;

    ParticleVertexOutput output;
    output.position = mul(float4(world, 1.0), frame.viewProjection);
    output.uv = input.uv;
    output.color = particle.color;
    return output;
}

struct ParticleFragmentInput
{
    [[vk::location(0)]] float2 uv;
    [[vk::location(1)]] float4 color;
};

struct ParticleFragmentOutput
{
    [[vk::location(0)]] float4 color : SV_Target;
};

[shader("fragment")]
ParticleFragmentOutput fragmentMain(ParticleFragmentInput input)
{
    float2 centered = input.uv * 2.0 - 1.0;
    float alpha = smoothstep(1.0, 0.65, dot(centered, centered));

    ParticleFragmentOutput output;
    output.color = float4(input.color.rgb, input.color.a * alpha);
    return output;
}
