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
    uint4 draw;
    float4 bladeShape;
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
    // originExtent.xy stores the horizontal XZ origin.  Keep the packed
    // half-float height on Y rather than accidentally interpreting Z as Y.
    return float3(cluster.originExtent.x + xz.x * cluster.originExtent.z,
                  y,
                  cluster.originExtent.y + xz.y * cluster.originExtent.z);
}

float grassPackedYaw(GrassPackedInstance instance)
{
    return grassUnorm16(instance.packedYRotation >> 16u) * 6.28318530718;
}

float grassPackedScale(GrassPackedInstance instance)
{
    return f16tof32(instance.packedScaleSeed & 0xffffu);
}

float grassPackedSeed(GrassPackedInstance instance)
{
    return float(instance.packedScaleSeed >> 16u) * (1.0 / 65535.0);
}

float3 grassRotateYawSinCos(float3 value, float sine, float cosine)
{
    return float3(value.x * cosine - value.z * sine, value.y,
                  value.x * sine + value.z * cosine);
}

float3 grassRotateYaw(float3 value, float yaw)
{
    float sine, cosine;
    sincos(yaw, sine, cosine);
    return grassRotateYawSinCos(value, sine, cosine);
}
