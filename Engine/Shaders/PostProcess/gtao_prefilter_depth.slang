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

// Each workgroup covers 16x16 mip0 pixels. Mip reductions alternate buffers
// so a phase never overwrites values that another thread in that phase reads.
groupshared float scratchA[8][8];
groupshared float scratchB[8][8];

float linearizeDepth(float z) {
    if (z >= 0.999999) return 65500.0;
    float4 view = mul(float4(0, 0, z, 1), settings.inverseProjection);
    return max(-view.z / max(view.w, 1e-6), 1e-4);
}

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

float reduceScratchA(uint2 p) {
    return depthMipFilter(scratchA[p.y * 2u][p.x * 2u], scratchA[p.y * 2u][p.x * 2u + 1u],
                          scratchA[p.y * 2u + 1u][p.x * 2u], scratchA[p.y * 2u + 1u][p.x * 2u + 1u]);
}

float reduceScratchB(uint2 p) {
    return depthMipFilter(scratchB[p.y * 2u][p.x * 2u], scratchB[p.y * 2u][p.x * 2u + 1u],
                          scratchB[p.y * 2u + 1u][p.x * 2u], scratchB[p.y * 2u + 1u][p.x * 2u + 1u]);
}

[numthreads(8, 8, 1)]
[shader("compute")]
void main(uint3 groupId : SV_GroupID, uint3 localId : SV_GroupThreadID) {
    uint width, height;
    hardwareDepth.GetDimensions(width, height);
    uint2 sourceBase = groupId.xy * 16u + localId.xy * 2u;

    // Keep all four mip0 values in registers while producing mip1.
    float d00 = linearizeDepth(hardwareDepth.Load(int3(min(sourceBase + uint2(0, 0), uint2(width, height) - 1u), 0)).r);
    float d10 = linearizeDepth(hardwareDepth.Load(int3(min(sourceBase + uint2(1, 0), uint2(width, height) - 1u), 0)).r);
    float d01 = linearizeDepth(hardwareDepth.Load(int3(min(sourceBase + uint2(0, 1), uint2(width, height) - 1u), 0)).r);
    float d11 = linearizeDepth(hardwareDepth.Load(int3(min(sourceBase + uint2(1, 1), uint2(width, height) - 1u), 0)).r);

    uint2 mip0Coord = sourceBase;
    if (all(mip0Coord < uint2(width, height))) mip0[mip0Coord] = d00;
    if (all(mip0Coord + uint2(1, 0) < uint2(width, height))) mip0[mip0Coord + uint2(1, 0)] = d10;
    if (all(mip0Coord + uint2(0, 1) < uint2(width, height))) mip0[mip0Coord + uint2(0, 1)] = d01;
    if (all(mip0Coord + uint2(1, 1) < uint2(width, height))) mip0[mip0Coord + uint2(1, 1)] = d11;

    float mip1Value = depthMipFilter(d00, d10, d01, d11);
    uint2 mip1Coord = groupId.xy * 8u + localId.xy;
    if (settings.mipCount > 1u) {
        scratchA[localId.y][localId.x] = mip1Value;
        uint mw = (width + 1u) / 2u, mh = (height + 1u) / 2u;
        if (all(mip1Coord < uint2(mw, mh))) mip1[mip1Coord] = mip1Value;
    }
    GroupMemoryBarrierWithGroupSync();

    if (settings.mipCount > 2u) {
        if (all(localId.xy < 4u)) {
            float v = reduceScratchA(localId.xy);
            scratchB[localId.y][localId.x] = v;
            uint2 coord = groupId.xy * 4u + localId.xy;
            uint mw = (width + 3u) / 4u, mh = (height + 3u) / 4u;
            if (all(coord < uint2(mw, mh))) mip2[coord] = v;
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (settings.mipCount > 3u) {
        if (all(localId.xy < 2u)) {
            float v = reduceScratchB(localId.xy);
            scratchA[localId.y][localId.x] = v;
            uint2 coord = groupId.xy * 2u + localId.xy;
            uint mw = (width + 7u) / 8u, mh = (height + 7u) / 8u;
            if (all(coord < uint2(mw, mh))) mip3[coord] = v;
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (settings.mipCount > 4u && all(localId.xy == 0u)) {
        float v = reduceScratchA(uint2(0, 0));
        uint mw = (width + 15u) / 16u, mh = (height + 15u) / 16u;
        if (all(groupId.xy < uint2(mw, mh))) mip4[groupId.xy] = v;
    }
}
