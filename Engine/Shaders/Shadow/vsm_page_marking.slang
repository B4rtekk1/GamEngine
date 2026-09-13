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
    uint padding[2];
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

float shadowTexelFootprintThreshold(const uint shadowQuality)
{
    // A larger threshold retains a finer clip level.  Higher settings can
    // afford that density and need it most where the filter footprint grows.
    if (shadowQuality == 3u) return 3.0;
    if (shadowQuality == 2u) return 2.0;
    return shadowQuality == 1u ? 1.5 : 1.0;
}

void markPageAndNeighbours(const uint encodedPage)
{
    if (encodedPage == 0xffffffffu) return;
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
        InterlockedOr(requestedPageBits[pageIndex >> 5], 1u << (pageIndex & 31));
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

groupshared uint groupPages[128];

[numthreads(8, 8, 1)]
[shader("compute")]
void main(uint3 threadId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID)
{
    const uint localIndex = groupThreadId.y * 8 + groupThreadId.x;
    groupPages[localIndex] = 0xffffffffu;
    groupPages[64 + localIndex] = 0xffffffffu;
    const bool inBounds = threadId.x < uniforms.depthWidth && threadId.y < uniforms.depthHeight;
    const float depth = inBounds ? receiverDepth.Load(int3(threadId.xy, 0)) : 1.0;
    if (inBounds && depth < 0.999999)
    {
        const float4 world = reconstructWorld(threadId.xy, depth);
        const uint2 neighbourPixelX = uint2(min(threadId.x + 1, uniforms.depthWidth - 1), threadId.y);
        const uint2 neighbourPixelY = uint2(threadId.x, min(threadId.y + 1, uniforms.depthHeight - 1));
        const float4 neighbourWorldX = reconstructWorld(neighbourPixelX, depth);
        const float4 neighbourWorldY = reconstructWorld(neighbourPixelY, depth);
        const uint pagesPerLevel = uniforms.pageCountPerAxis * uniforms.pageCountPerAxis;
        uint selectedLevel = 0xffffffffu;
        // Select the finest clip level whose virtual texel is no smaller
        // than the receiver's one-pixel footprint.  Mark one coarser level
        // as a cache-miss fallback, not all seven clipmaps.
        [loop] for (uint level = 0; level < uniforms.clipLevelCount; ++level)
        {
            const float4 lightClip = mul(world, uniforms.clipMatrices[level]);
            const float4 neighbourClipX = mul(neighbourWorldX, uniforms.clipMatrices[level]);
            const float4 neighbourClipY = mul(neighbourWorldY, uniforms.clipMatrices[level]);
            if (abs(lightClip.w) < 1.0e-6 || abs(neighbourClipX.w) < 1.0e-6 ||
                abs(neighbourClipY.w) < 1.0e-6) continue;
            const float2 lightUv = lightClip.xy / lightClip.w * 0.5 + 0.5;
            if (any(lightUv < 0.0) || any(lightUv >= 1.0)) continue;
            const float2 neighbourUvX = neighbourClipX.xy / neighbourClipX.w * 0.5 + 0.5;
            const float2 neighbourUvY = neighbourClipY.xy / neighbourClipY.w * 0.5 + 0.5;
            const float footprintInTexels = max(length(neighbourUvX - lightUv),
                                                length(neighbourUvY - lightUv)) *
                                            float(uniforms.virtualResolution);
            if (footprintInTexels <= shadowTexelFootprintThreshold(uniforms.shadowQuality) ||
                level + 1 == uniforms.clipLevelCount)
            {
                const uint2 page = min(uint2(lightUv * uniforms.pageCountPerAxis), uniforms.pageCountPerAxis - 1);
                groupPages[localIndex] = level * pagesPerLevel + page.y * uniforms.pageCountPerAxis + page.x;
                const uint fallbackLevel = min(level + 1, uniforms.clipLevelCount - 1);
                const float4 fallbackClip = mul(world, uniforms.clipMatrices[fallbackLevel]);
                const float2 fallbackUv = fallbackClip.xy / fallbackClip.w * 0.5 + 0.5;
                if (all(fallbackUv >= 0.0) && all(fallbackUv < 1.0) && fallbackLevel != level)
                {
                    const uint2 fallbackPage = min(uint2(fallbackUv * uniforms.pageCountPerAxis), uniforms.pageCountPerAxis - 1);
                    groupPages[64 + localIndex] = fallbackLevel * pagesPerLevel + fallbackPage.y * uniforms.pageCountPerAxis + fallbackPage.x;
                }
                break;
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (localIndex != 0) return;
    // Nearby receivers overwhelmingly map to the same page.  Collapse the
    // complete 8x8 tile before touching the tiny global request bitset.
    [unroll] for (uint index = 0; index < 128; ++index)
    {
        const uint page = groupPages[index];
        bool first = page != 0xffffffffu;
        [unroll] for (uint prior = 0; prior < index; ++prior)
            first = first && groupPages[prior] != page;
        if (first) markPageAndNeighbours(page);
    }
}
