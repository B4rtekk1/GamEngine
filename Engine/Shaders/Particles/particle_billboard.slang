import Particles.ParticleTypes;

#ifndef PARTICLE_OUTPUT_VELOCITY
#define PARTICLE_OUTPUT_VELOCITY 1
#endif

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
#if PARTICLE_OUTPUT_VELOCITY
    [[vk::location(2)]] noperspective float2 currentNdc;
    [[vk::location(3)]] float4 previousClip;
#endif
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
#if PARTICLE_OUTPUT_VELOCITY
    // Simulation advances position by velocity * deltaTime before rendering.
    // Reconstruct the prior billboard using the previous camera basis too.
    float3 previousWorld = particle.positionLife.xyz - particle.velocitySize.xyz * frame.deltaTime +
        (frame.previousCameraRight * input.position.x + frame.previousCameraUp * input.position.y) *
        particle.velocitySize.w;
    output.currentNdc = output.position.xy / output.position.w;
    output.previousClip = mul(float4(previousWorld, 1.0), frame.previousViewProjection);
#endif
    return output;
}

struct ParticleFragmentInput
{
    [[vk::location(0)]] float2 uv;
    [[vk::location(1)]] float4 color;
#if PARTICLE_OUTPUT_VELOCITY
    [[vk::location(2)]] noperspective float2 currentNdc;
    [[vk::location(3)]] float4 previousClip;
#endif
};

struct ParticleFragmentOutput
{
    [[vk::location(0)]] float4 color : SV_Target;
#if PARTICLE_OUTPUT_VELOCITY
    [[vk::location(1)]] float2 velocity : SV_Target1;
#endif
};

[shader("fragment")]
ParticleFragmentOutput fragmentMain(ParticleFragmentInput input)
{
    float2 centered = input.uv * 2.0 - 1.0;
    float alpha = smoothstep(1.0, 0.65, dot(centered, centered));

    ParticleFragmentOutput output;
    output.color = float4(input.color.rgb, input.color.a * alpha);
#if PARTICLE_OUTPUT_VELOCITY
    output.velocity = (input.currentNdc - input.previousClip.xy / input.previousClip.w) * 0.5;
#endif
    return output;
}
