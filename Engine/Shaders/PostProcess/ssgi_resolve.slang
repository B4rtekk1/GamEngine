// Spatial denoise fused with the depth-aware upsample to full resolution. A
// 3x3 Gaussian over the accumulated half-resolution GI (two full-resolution
// pixels each way), restricted to texels at the receiver's depth, removes the
// noise temporal accumulation leaves without bleeding across silhouettes.
import PostProcess.SsgiCommon;

[[vk::binding(0,0)]] Sampler2D accumulatedGi;
[[vk::binding(1,0)]] Sampler2D sceneDepth;
[[vk::binding(2,0)]] RWTexture2D<float4> fullGi;
struct Settings {
    float4 depthToView;
    float intensity;
    float padding0, padding1, padding2;
};
[[vk::push_constant]] ConstantBuffer<Settings> settings;

static const int KernelRadius = 1;
// An 8x8 output group spans at most five half-resolution texels per axis.
static const int TileSize = 5 + 2 * KernelRadius;
groupshared float4 giTile[TileSize][TileSize];

[numthreads(8,8,1)] [shader("compute")]
void main(uint3 groupId:SV_GroupID,uint3 threadId:SV_GroupThreadID,uint3 id:SV_DispatchThreadID){
    uint hw, hh;
    accumulatedGi.GetDimensions(hw, hh);
    uint w, h;
    fullGi.GetDimensions(w, h);
    const float2 scale = float2(hw, hh) / float2(w, h);
    const int2 tileBase = int2(floor((float2(groupId.xy * 8u) + 0.5) * scale - 0.5)) - KernelRadius;
    const int2 maxHalfPixel = int2(hw, hh) - 1;
    for (uint i = threadId.y * 8u + threadId.x; i < uint(TileSize * TileSize); i += 64u) {
        const int2 t = int2(i % uint(TileSize), i / uint(TileSize));
        giTile[t.y][t.x] = accumulatedGi.Load(int3(clamp(tileBase + t, int2(0, 0), maxHalfPixel), 0));
    }
    GroupMemoryBarrierWithGroupSync();
    if (any(id.xy >= uint2(w, h))) return;

    const float depth = ssgiLinearizeDepth(sceneDepth.Load(int3(id.xy, 0)).r, settings.depthToView);
    if (depth >= SsgiBackgroundDepth) {
        fullGi[id.xy] = 0.0;
        return;
    }
    const float2 halfPosition = (float2(id.xy) + 0.5) * scale - 0.5;
    const int2 nearest = int2(round(halfPosition));
    const float depthScale = 1.0 / (0.02 * depth + 0.01);
    float3 sum = 0.0;
    float weightSum = 0.0, bestDepthError = 1e30;
    float3 bestGi = 0.0;
    [unroll] for (int y = -KernelRadius; y <= KernelRadius; ++y)
        [unroll] for (int x = -KernelRadius; x <= KernelRadius; ++x) {
            const int2 texel = nearest + int2(x, y);
            const int2 t = clamp(texel - tileBase, int2(0, 0), int2(TileSize - 1, TileSize - 1));
            const float4 tap = giTile[t.y][t.x];
            const float depthError = abs(tap.a - depth);
            if (depthError < bestDepthError) {
                bestDepthError = depthError;
                bestGi = tap.rgb;
            }
            const float2 offset = float2(texel) - halfPosition;
            // exp(-0.6 d^2 - e) as a single exp2.
            const float weight = exp2(-0.866 * dot(offset, offset) - 1.4427 * depthError * depthScale);
            sum += tap.rgb * weight;
            weightSum += weight;
        }
    const float3 gi = weightSum > 1e-4 ? sum / weightSum : bestGi;
    fullGi[id.xy] = float4(max(gi, 0.0) * settings.intensity, 1.0);
}
