import Particles.ParticleTypes;
import Particles.SimulationMath;
import Particles.SimulationTypes;

[[vk::binding(0, 0)]] RWStructuredBuffer<Particle> particles;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> activeIndices;
[[vk::binding(3, 0)]] RWStructuredBuffer<ParticleDrawCommand> drawCommands;
[[vk::binding(4, 0)]] StructuredBuffer<ParticleCollider> colliders;
[[vk::push_constant]] ConstantBuffer<ParticleSimulationData> sim;

[numthreads(256, 1, 1)]
[shader("compute")]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint id = dispatchThreadId.x;
    if (id >= sim.maxParticles)
        return;

    Particle particle = particles[id];
    float dt = min(sim.deltaTime, 0.1);

    if (particle.positionLife.w > 0.0)
    {
        float age = clamp(1.0 - particle.positionLife.w /
                          max(particle.spawnData.z, 0.001), 0.0, 1.0);
        float expansionAge = 1.0 - age;
        float expansion = 1.0 - expansionAge * expansionAge *
                          (0.6 + 0.4 * expansionAge);
        particle.velocitySize.w = lerp(particle.spawnData.x,
                                       particle.spawnData.y, expansion);

        float3 swirl = curlNoise(particle.positionLife.xyz * 0.72 +
                                 float3(2.1, 0.0, 4.6)) * 0.20;
        swirl.y *= 0.35;
        particle.velocitySize.xyz +=
            (float3(0.0, sim.smokeDynamics.x, 0.0) + swirl * sim.smokeDynamics.z) * dt;
        particle.velocitySize.xyz *= 1.0 / (1.0 + sim.smokeDynamics.y * dt);

        float3 nextPosition = particle.positionLife.xyz + particle.velocitySize.xyz * dt;
        for (uint colliderIndex = 0; colliderIndex < sim.colliderCount; ++colliderIndex)
        {
            float3 center = colliders[colliderIndex].center.xyz;
            float3 extents = colliders[colliderIndex].halfExtents.xyz +
                             max(sim.smokeDynamics.w, particle.velocitySize.w * 0.35);
            float3 surfaceNormal;
            float distance = expandedAabbDistance(particle.positionLife.xyz, center,
                                                  extents, surfaceNormal);
            float flowRange = max(0.85, particle.velocitySize.w * 4.0);
            float proximity = 1.0 - smoothstep(0.0, flowRange, max(distance, 0.0));
            if (proximity <= 0.0)
                continue;

            float3 side = cross(float3(0.0, 1.0, 0.0), surfaceNormal);
            if (dot(side, side) < 0.001)
                side = cross(surfaceNormal, float3(1.0, 0.0, 0.0));
            side = normalize(side);
            float branch = (particleHash(id + colliderIndex * 1664525) & 1) == 0
                ? -1.0 : 1.0;
            float3 around = normalize(side * branch + float3(0.0, 0.38, 0.0));
            float incoming = max(0.0, -dot(particle.velocitySize.xyz, surfaceNormal));
            float flowStrength = 2.4 + 3.0 * length(particle.velocitySize.xyz);
            particle.velocitySize.xyz +=
                (surfaceNormal * incoming * 2.0 + around * flowStrength) * proximity * dt;

            nextPosition = particle.positionLife.xyz + particle.velocitySize.xyz * dt;
            float3 relativePosition = nextPosition - center;
            if (all(abs(relativePosition) < extents))
            {
                float3 margin = extents - abs(relativePosition);
                float3 penetrationNormal =
                    (margin.x <= margin.y && margin.x <= margin.z)
                    ? float3(relativePosition.x >= 0.0 ? 1.0 : -1.0, 0.0, 0.0)
                    : (margin.y <= margin.z
                       ? float3(0.0, relativePosition.y >= 0.0 ? 1.0 : -1.0, 0.0)
                       : float3(0.0, 0.0, relativePosition.z >= 0.0 ? 1.0 : -1.0));
                float penetration = min(min(margin.x, margin.y), margin.z);
                nextPosition += penetrationNormal * (penetration + 0.001);
                float intoSurface = dot(particle.velocitySize.xyz, penetrationNormal);
                if (intoSurface < 0.0)
                    particle.velocitySize.xyz -= penetrationNormal * intoSurface;
                particle.velocitySize.xyz *= 0.82;
            }
        }

        particle.positionLife.xyz = nextPosition;
        particle.positionLife.w -= dt;
        float fadeIn = smoothstep(0.0, 0.12, age);
        float fadeOut = smoothstep(0.0, 1.15, particle.positionLife.w);
        particle.color.a = particle.spawnData.w * fadeIn * fadeOut;
    }

    uint relative = (id + sim.maxParticles - sim.spawnStart) % sim.maxParticles;
    if (relative < sim.spawnCount)
    {
        uint state = sim.spawnSeed + relative;
        float angle = particleRandom(state) * 6.28318530718;
        float radius = sqrt(particleRandom(state)) *
                       max(sim.minVelocityMinSize.w, 0.08) * 1.35;
        float3 spawnOffset = float3(cos(angle) * radius,
                                    particleRandom(state) * radius * 0.22,
                                    sin(angle) * radius);
        float lifetime = lerp(sim.emitterPositionMinLife.w,
                              sim.maxLifeMaxSize.x, particleRandom(state));
        float initialSize = lerp(sim.minVelocityMinSize.w,
                                 sim.maxLifeMaxSize.y, particleRandom(state));
        float finalSize = initialSize * lerp(1.65, 2.35, particleRandom(state));
        particle.positionLife = float4(sim.emitterPositionMinLife.xyz + spawnOffset,
                                       lifetime);
        particle.velocitySize = float4(
            lerp(sim.minVelocityMinSize.xyz, sim.maxVelocity.xyz,
                 float3(particleRandom(state), particleRandom(state), particleRandom(state))),
            initialSize);
        particle.color = sim.color;
        particle.spawnData = float4(initialSize, finalSize, lifetime, sim.color.a);
    }

    particles[id] = particle;
    if (particle.positionLife.w > 0.0)
    {
        uint compactIndex;
        InterlockedAdd(drawCommands[0].instanceCount, 1, compactIndex);
        activeIndices[compactIndex] = id;
    }
}
