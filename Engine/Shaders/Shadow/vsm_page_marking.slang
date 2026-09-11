// Receiver-driven virtual-shadow page requests.  This pass deliberately
// consumes depth rather than the scene object list: sky pixels do no work and
// a view with no receivers cannot turn an O(object-count) CPU loop into work.

struct VsmPageMarkingUniforms
{
    float4x4 inverseViewProjection;
    float4x4 clipMatrices[7];
    uint pageCountPerAxis;
    uint clipLevelCount;
    uint depthWidth;
    uint depthHeight;
};

[[vk::binding(0, 0)]] Texture2D<float> receiverDepth;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> requestedPageBits;
[[vk::binding(2, 0)]] ConstantBuffer<VsmPageMarkingUniforms> uniforms;

void markPage(const uint level, const float2 uv)
{
    if (any(uv < 0.0) || any(uv >= 1.0)) return;
    const uint2 page = min(uint2(uv * uniforms.pageCountPerAxis),
                           uniforms.pageCountPerAxis - 1);
    const uint pageIndex = level * uniforms.pageCountPerAxis * uniforms.pageCountPerAxis +
                           page.y * uniforms.pageCountPerAxis + page.x;
    InterlockedOr(requestedPageBits[pageIndex >> 5], 1u << (pageIndex & 31));
}

[numthreads(8, 8, 1)]
[shader("compute")]
void main(uint3 threadId : SV_DispatchThreadID)
{
    if (threadId.x >= uniforms.depthWidth || threadId.y >= uniforms.depthHeight) return;

    // A cleared depth pixel has no receiver and must never allocate VSM pages.
    const float depth = receiverDepth.Load(int3(threadId.xy, 0));
    if (depth >= 0.999999) return;

    const float2 uv = (float2(threadId.xy) + 0.5) /
                      float2(uniforms.depthWidth, uniforms.depthHeight);
    const float4 clip = float4(uv * 2.0 - 1.0, depth, 1.0);
    const float4 worldHomogeneous = mul(clip, uniforms.inverseViewProjection);
    if (abs(worldHomogeneous.w) < 1.0e-6) return;
    const float4 world = float4(worldHomogeneous.xyz / worldHomogeneous.w, 1.0);

    [loop]
    for (uint level = 0; level < uniforms.clipLevelCount; ++level)
    {
        const float4 lightClip = mul(world, uniforms.clipMatrices[level]);
        if (abs(lightClip.w) < 1.0e-6) continue;
        const float2 lightUv = lightClip.xy / lightClip.w * 0.5 + 0.5;
        // Request a 3x3 neighbourhood to cover depth discontinuities and
        // bilinear/filter footprints at virtual-page boundaries.
        const float pageSize = 1.0 / float(uniforms.pageCountPerAxis);
        markPage(level, lightUv);
        markPage(level, lightUv + float2(pageSize, 0.0));
        markPage(level, lightUv + float2(-pageSize, 0.0));
        markPage(level, lightUv + float2(0.0, pageSize));
        markPage(level, lightUv + float2(0.0, -pageSize));
    }
}
