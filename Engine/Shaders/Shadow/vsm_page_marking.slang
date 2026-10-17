import Common.ShadowLod;

// Receiver-driven virtual-shadow page requests.  This pass deliberately
// consumes depth rather than the scene object list: sky pixels do no work and
// a view with no receivers cannot turn an O(object-count) CPU loop into work.

struct VsmPageMarkingUniforms
{
    float4x4 inverseViewProjection;
    float4x4 clipMatrices[7];
    uint pageCountPerAxis;
    uint virtualResolution;
    uint clipLevelCount;
    uint depthWidth;
    uint depthHeight;
    uint shadowQuality;
    uint feedbackStride;
    uint padding;
};

[[vk::binding(0, 0)]] Texture2D<float> receiverDepth;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> requestedPageBits;
[[vk::binding(2, 0)]] ConstantBuffer<VsmPageMarkingUniforms> uniforms;

// One shared bitset per workgroup; duplicate requests coalesce in parallel.
static const uint REQUEST_WORD_COUNT = (7u * 32u * 32u + 31u) / 32u;
groupshared uint groupPageBits[REQUEST_WORD_COUNT];

void markPageAndNeighbours(const uint encodedPage)
{
    if (encodedPage == 0xffffffffu) return;
    // With 32 pages per axis each word is one complete row. Coalesce only
    // centres here; the flush dilates rows after all receivers have finished.
    // This replaces nine contended shared atomics per request with one.
    if (uniforms.pageCountPerAxis == 32u)
    {
        InterlockedOr(groupPageBits[encodedPage >> 5], 1u << (encodedPage & 31));
        return;
    }
    const uint pagesPerLevel = uniforms.pageCountPerAxis * uniforms.pageCountPerAxis;
    const uint level = encodedPage / pagesPerLevel;
    const uint local = encodedPage - level * pagesPerLevel;
    const int2 page = int2(local % uniforms.pageCountPerAxis, local / uniforms.pageCountPerAxis);
    // Dilation is deliberately page based, rather than performed per receiver
    // pixel.  This is the complete 3x3 neighbourhood (including corners).
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        int2 neighbour = page + int2(x, y);
        if (any(neighbour < int2(0)) || any(neighbour >= int2(uniforms.pageCountPerAxis))) continue;
        const uint pageIndex = level * pagesPerLevel + uint(neighbour.y) * uniforms.pageCountPerAxis + uint(neighbour.x);
        InterlockedOr(groupPageBits[pageIndex >> 5], 1u << (pageIndex & 31));
    }
}

float4 reconstructWorld(const uint2 pixel, const float depth)
{
    const float2 uv = (float2(pixel) + 0.5) / float2(uniforms.depthWidth, uniforms.depthHeight);
    const float4 clip = float4(uv * 2.0 - 1.0, depth, 1.0);
    const float4 homogeneous = mul(clip, uniforms.inverseViewProjection);
    return abs(homogeneous.w) < 1.0e-6 ? float4(0.0, 0.0, 0.0, 0.0)
                                         : float4(homogeneous.xyz / homogeneous.w, 1.0);
}

[numthreads(8, 8, 1)]
[shader("compute")]
void main(uint3 threadId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID)
{
    const uint localIndex = groupThreadId.y * 8 + groupThreadId.x;
    for (uint word = localIndex; word < REQUEST_WORD_COUNT; word += 64u)
        groupPageBits[word] = 0u;
    GroupMemoryBarrierWithGroupSync();
    const uint stride = max(uniforms.feedbackStride, 1u);
    const bool inBounds = threadId.x * stride < uniforms.depthWidth &&
                          threadId.y * stride < uniforms.depthHeight;
    const uint2 lastPixel = uint2(uniforms.depthWidth, uniforms.depthHeight) - 1u;
    const uint2 blockOrigin = min(threadId.xy * stride, lastPixel);
    const uint2 samplePixel = min(blockOrigin + stride / 2u, lastPixel);
    const uint2 blockRight = min(blockOrigin + uint2(stride - 1u, 0u), lastPixel);
    const uint2 blockDown = min(blockOrigin + uint2(0u, stride - 1u), lastPixel);
    const uint2 blockCorner = min(blockOrigin + uint2(stride - 1u, stride - 1u), lastPixel);
    const float depth0 = inBounds ? receiverDepth.Load(int3(blockOrigin, 0)) : 1.0;
    const float depth1 = inBounds ? receiverDepth.Load(int3(blockRight, 0)) : 1.0;
    const float depth2 = inBounds ? receiverDepth.Load(int3(blockDown, 0)) : 1.0;
    const float depth3 = inBounds ? receiverDepth.Load(int3(blockCorner, 0)) : 1.0;
    const float depth = min(min(depth0, depth1), min(depth2, depth3));
    if (inBounds && depth < 0.999999)
    {
        const uint2 receiverPixel = depth == depth0 ? blockOrigin :
            (depth == depth1 ? blockRight : (depth == depth2 ? blockDown : blockCorner));
        const float4 world = reconstructWorld(receiverPixel, depth);
        // Use derivatives over the reduced-resolution sample footprint. The
        // page-neighbour dilation keeps this conservative at sample boundaries.
        const uint2 rightPixel = uint2(min(receiverPixel.x + stride, uniforms.depthWidth - 1u), receiverPixel.y);
        const uint2 downPixel = uint2(receiverPixel.x, min(receiverPixel.y + stride, uniforms.depthHeight - 1u));
        const float rightDepth = receiverDepth.Load(int3(rightPixel, 0));
        const float downDepth = receiverDepth.Load(int3(downPixel, 0));
        const float4 rightWorld = reconstructWorld(rightPixel, rightDepth < 0.999999 ? rightDepth : depth);
        const float4 downWorld = reconstructWorld(downPixel, downDepth < 0.999999 ? downDepth : depth);
        const uint pagesPerLevel = uniforms.pageCountPerAxis * uniforms.pageCountPerAxis;
        // Select the finest clip level whose virtual texel is no smaller
        // than the receiver's one-pixel footprint.  Mark one coarser level
        // as a cache-miss fallback, not all seven clipmaps.
        [loop] for (uint level = 0; level < uniforms.clipLevelCount; ++level)
        {
            const float4 lightClip = mul(world, uniforms.clipMatrices[level]);
            const float4 rightClip = mul(rightWorld, uniforms.clipMatrices[level]);
            const float4 downClip = mul(downWorld, uniforms.clipMatrices[level]);
            if (abs(lightClip.w) < 1.0e-6 || abs(rightClip.w) < 1.0e-6 ||
                abs(downClip.w) < 1.0e-6) continue;
            const float2 lightUv = lightClip.xy / lightClip.w * 0.5 + 0.5;
            if (any(lightUv < 0.0) || any(lightUv >= 1.0)) continue;
            const float2 rightUv = rightClip.xy / rightClip.w * 0.5 + 0.5;
            const float2 downUv = downClip.xy / downClip.w * 0.5 + 0.5;
            const float2 dx = rightUv - lightUv;
            const float2 dy = downUv - lightUv;
            const float footprintInTexels = shadowTexelFootprintFromGradients(dx, dx, dy, dy);
            if (footprintInTexels <= shadowTexelFootprintThreshold(uniforms.shadowQuality) ||
                level + 1 == uniforms.clipLevelCount)
            {
                const uint2 page = min(uint2(lightUv * uniforms.pageCountPerAxis), uniforms.pageCountPerAxis - 1);
                markPageAndNeighbours(level * pagesPerLevel + page.y * uniforms.pageCountPerAxis + page.x);
                const uint fallbackLevel = min(level + 1, uniforms.clipLevelCount - 1);
                const float4 fallbackClip = mul(world, uniforms.clipMatrices[fallbackLevel]);
                const float2 fallbackUv = fallbackClip.xy / fallbackClip.w * 0.5 + 0.5;
                if (all(fallbackUv >= 0.0) && all(fallbackUv < 1.0) && fallbackLevel != level)
                {
                    const uint2 fallbackPage = min(uint2(fallbackUv * uniforms.pageCountPerAxis), uniforms.pageCountPerAxis - 1);
                    markPageAndNeighbours(fallbackLevel * pagesPerLevel + fallbackPage.y * uniforms.pageCountPerAxis + fallbackPage.x);
                }
                break;
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    // Flush each nonempty word once. All 64 lanes participate; there is no
    // quadratic serial scan or wave-size-dependent correctness assumption.
    for (uint word = localIndex; word < REQUEST_WORD_COUNT; word += 64u)
    {
        uint bits = groupPageBits[word];
        if (uniforms.pageCountPerAxis == 32u)
        {
            // Keep dilation inside its clip level, including corner pages.
            const uint row = word & 31u;
            if (row > 0u) bits |= groupPageBits[word - 1u];
            if (row < 31u) bits |= groupPageBits[word + 1u];
            bits |= (bits << 1u) | (bits >> 1u);
        }
        if (bits != 0u) InterlockedOr(requestedPageBits[word], bits);
    }
}
