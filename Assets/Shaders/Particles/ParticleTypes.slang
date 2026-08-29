struct Particle
{
    float4 positionLife;
    float4 velocitySize;
    float4 color;
    float4 spawnData;
};

struct ParticleFrameData
{
    float4x4 viewProjection;
    float3 cameraRight;
    float3 cameraUp;
};
