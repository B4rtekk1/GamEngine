struct ParticleDrawCommand
{
    uint vertexCount;
    uint instanceCount;
    uint firstVertex;
    uint firstInstance;
};

struct ParticleCollider
{
    float4 center;
    float4 halfExtents;
};

struct ParticleSimulationData
{
    float deltaTime;
    uint spawnStart;
    uint spawnCount;
    uint maxParticles;
    uint spawnSeed;
    uint colliderCount;
    float padding1;
    float padding2;
    float4 emitterPositionMinLife;
    float4 minVelocityMinSize;
    float4 maxVelocity;
    float4 color;
    float4 maxLifeMaxSize;
    float4 smokeDynamics;
};
