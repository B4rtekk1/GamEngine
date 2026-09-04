// Must remain byte-for-byte compatible with Engine::GPUGrassInstance and
// Engine::GPUGrassCluster. This module is intentionally independent of the
// generic RendererInstanceData contract.
struct GrassPackedInstance
{
    uint packedXZ;
    uint packedYRotation;
    uint packedScaleSeed;
    uint flags;
};

struct GrassPackedCluster
{
    float4 originExtent;
    uint4 instanceRange;
};

float grassUnorm16(uint value)
{
    return float(value & 0xffffu) * (1.0 / 65535.0);
}

float3 grassPackedWorldPosition(GrassPackedInstance instance,
                                GrassPackedCluster cluster)
{
    float2 xz = float2(grassUnorm16(instance.packedXZ),
                        grassUnorm16(instance.packedXZ >> 16u));
    float y = f16tof32(instance.packedYRotation & 0xffffu);
    return float3(cluster.originExtent.xy + xz * cluster.originExtent.z, y);
}

float grassPackedYaw(GrassPackedInstance instance)
{
    return grassUnorm16(instance.packedYRotation >> 16u) * 6.28318530718;
}

float grassPackedScale(GrassPackedInstance instance)
{
    return f16tof32(instance.packedScaleSeed & 0xffffu);
}

float3 grassRotateYaw(float3 value, float yaw)
{
    float sine = sin(yaw);
    float cosine = cos(yaw);
    return float3(value.x * cosine - value.z * sine, value.y,
                  value.x * sine + value.z * cosine);
}
