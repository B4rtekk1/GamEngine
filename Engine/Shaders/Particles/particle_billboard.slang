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
    [[vk::location(4)]] nointerpolation float glass;
#if PARTICLE_OUTPUT_VELOCITY
    [[vk::location(2)]] noperspective float2 currentNdc;
    [[vk::location(3)]] float4 previousClip;
#endif
};

[shader("vertex")]
ParticleVertexOutput vertexMain(ParticleVertexInput input, uint instanceIndex : SV_InstanceID)
{
    Particle particle = particles[activeIndices[instanceIndex]];
    bool glass = particle.spawnData.y < 0;
    float angle = glass ? (particle.spawnData.z-particle.positionLife.w) * 7.0 + float(activeIndices[instanceIndex]) : 0;
    float2 rotated = float2(input.position.x*cos(angle)-input.position.y*sin(angle),
                           input.position.x*sin(angle)+input.position.y*cos(angle));
    if (glass) rotated.y *= 0.55;
    float3 world = particle.positionLife.xyz +
        (frame.cameraRight * rotated.x + frame.cameraUp * rotated.y) *
        particle.velocitySize.w;

    ParticleVertexOutput output;
    output.position = mul(float4(world, 1.0), frame.viewProjection);
    output.uv = input.uv;
    output.color = particle.color;
    output.glass = glass ? 1.0 : 0.0;
#if PARTICLE_OUTPUT_VELOCITY
    // Simulation advances position by velocity * deltaTime before rendering.
    // Reconstruct the prior billboard using the previous camera basis too.
    float previousAngle = glass ? angle - frame.deltaTime * 7.0 : 0;
    float2 previousRotated = float2(input.position.x*cos(previousAngle)-input.position.y*sin(previousAngle),
                                   input.position.x*sin(previousAngle)+input.position.y*cos(previousAngle));
    if (glass) previousRotated.y *= 0.55;
    float3 previousWorld = particle.positionLife.xyz - particle.velocitySize.xyz * frame.deltaTime +
        (frame.previousCameraRight * previousRotated.x + frame.previousCameraUp * previousRotated.y) *
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
    [[vk::location(4)]] nointerpolation float glass;
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
    float alpha = input.glass > 0.5 ? step(abs(centered.x) * 0.5 + abs(centered.y), 1.0) :
        smoothstep(1.0, 0.65, dot(centered, centered));

    ParticleFragmentOutput output;
    output.color = float4(input.color.rgb, input.color.a * alpha);
#if PARTICLE_OUTPUT_VELOCITY
    output.velocity = (input.currentNdc - input.previousClip.xy / input.previousClip.w) * 0.5;
#endif
    return output;
}
