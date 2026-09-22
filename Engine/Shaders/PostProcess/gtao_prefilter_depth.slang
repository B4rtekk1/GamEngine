[[vk::binding(0, 0)]] Sampler2D hardwareDepth;
[[vk::binding(1, 0)]] RWTexture2D<float> mip0;
[[vk::binding(2, 0)]] RWTexture2D<float> mip1;
[[vk::binding(3, 0)]] RWTexture2D<float> mip2;
[[vk::binding(4, 0)]] RWTexture2D<float> mip3;
[[vk::binding(5, 0)]] RWTexture2D<float> mip4;

struct Settings {
    float4x4 inverseProjection;
    float effectRadius;
    float effectFalloffRange;
    uint mipCount;
};
[[vk::push_constant]] ConstantBuffer<Settings> settings;

groupshared float tile[16][16];

float depthMipFilter(float d0, float d1, float d2, float d3) {
    float maxDepth = max(max(d0, d1), max(d2, d3));
    const float depthRangeScaleFactor = 0.75;
    float effectRadius = depthRangeScaleFactor * settings.effectRadius * 0.5 * 1.457;
    float falloffRange = settings.effectFalloffRange * effectRadius;
    float falloffFrom = effectRadius * (1.0 - settings.effectFalloffRange);
    float falloffMul = -1.0 / falloffRange;
    float falloffAdd = falloffFrom / falloffRange + 1.0;
    float w0 = saturate((maxDepth - d0) * falloffMul + falloffAdd);
    float w1 = saturate((maxDepth - d1) * falloffMul + falloffAdd);
    float w2 = saturate((maxDepth - d2) * falloffMul + falloffAdd);
    float w3 = saturate((maxDepth - d3) * falloffMul + falloffAdd);
    float weightSum = w0 + w1 + w2 + w3;
    return (w0 * d0 + w1 * d1 + w2 * d2 + w3 * d3) / weightSum;
}

float reduceTile(uint2 p) {
    return depthMipFilter(tile[p.y * 2u][p.x * 2u], tile[p.y * 2u][p.x * 2u + 1u],
                          tile[p.y * 2u + 1u][p.x * 2u], tile[p.y * 2u + 1u][p.x * 2u + 1u]);
}

[numthreads(16, 16, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID, uint3 groupId : SV_GroupID, uint3 localId : SV_GroupThreadID) {
    uint width, height;
    hardwareDepth.GetDimensions(width, height);
    uint2 source = min(id.xy, uint2(width, height) - 1u);
    float z = hardwareDepth.Load(int3(source, 0)).r;
    if (z >= 0.999999) {
        tile[localId.y][localId.x] = 65500.0;
    } else {
        float4 view = mul(float4(0, 0, z, 1), settings.inverseProjection);
        tile[localId.y][localId.x] = max(-view.z / max(view.w, 1e-6), 1e-4);
    }

    uint2 mip0Coord = id.xy;
    if (all(mip0Coord < uint2(width, height))) mip0[mip0Coord] = tile[localId.y][localId.x];
    GroupMemoryBarrierWithGroupSync();

    uint2 mip1Coord = groupId.xy * 8u + localId.xy;
    if (all(localId.xy < 8u)) {
        tile[localId.y][localId.x] = reduceTile(localId.xy);
        if (settings.mipCount > 1u) {
            uint mw = (width + 1u) / 2u, mh = (height + 1u) / 2u;
            if (all(mip1Coord < uint2(mw, mh))) mip1[mip1Coord] = tile[localId.y][localId.x];
        }
    }
    GroupMemoryBarrierWithGroupSync();

    uint2 mip2Coord = groupId.xy * 4u + localId.xy;
    if (all(localId.xy < 4u)) {
        tile[localId.y][localId.x] = reduceTile(localId.xy);
        if (settings.mipCount > 2u) {
            uint mw = (width + 3u) / 4u, mh = (height + 3u) / 4u;
            if (all(mip2Coord < uint2(mw, mh))) mip2[mip2Coord] = tile[localId.y][localId.x];
        }
    }
    GroupMemoryBarrierWithGroupSync();

    uint2 mip3Coord = groupId.xy * 2u + localId.xy;
    if (all(localId.xy < 2u)) {
        tile[localId.y][localId.x] = reduceTile(localId.xy);
        if (settings.mipCount > 3u) {
            uint mw = (width + 7u) / 8u, mh = (height + 7u) / 8u;
            if (all(mip3Coord < uint2(mw, mh))) mip3[mip3Coord] = tile[localId.y][localId.x];
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (localId.x == 0u && localId.y == 0u && settings.mipCount > 4u) {
        uint mw = (width + 15u) / 16u, mh = (height + 15u) / 16u;
        if (all(groupId.xy < uint2(mw, mh))) mip4[groupId.xy] = reduceTile(uint2(0, 0));
    }
}
