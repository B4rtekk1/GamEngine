// Stage 2 VSM allocator contract. This pass consumes the compacted GPU request
// list and writes pending work. Publication into the sampling page table stays
// in the later commit pass, after shadow rasterization.

struct VsmPageAllocatorUniforms
{
    float4x4 clipMatrices[7];
    uint pageCountPerAxis;
    uint physicalPageCount;
    uint frameIndex;
    uint maxPageWork;
};

struct PhysicalPageMetadata
{
    uint virtualPage;
    uint lastUsedFrame;
    uint flags;
    uint padding;
};

struct PageWork
{
    float4x4 viewProjection;
    uint drawSlot;
    uint clipLevel;
    uint physicalPage;
    uint virtualPage;
};

[[vk::binding(0, 0)]] StructuredBuffer<uint> compactedPageKeys;
[[vk::binding(1, 0)]] StructuredBuffer<uint> compactedPageCount;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> virtualToPhysical;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> physicalToVirtual;
[[vk::binding(4, 0)]] RWStructuredBuffer<PhysicalPageMetadata> physicalMetadata;
[[vk::binding(5, 0)]] RWStructuredBuffer<uint> freePages;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> freePageCount;
[[vk::binding(7, 0)]] RWStructuredBuffer<PageWork> pageWork;
[[vk::binding(8, 0)]] RWStructuredBuffer<uint> pageWorkCount;
[[vk::binding(9, 0)]] ConstantBuffer<VsmPageAllocatorUniforms> uniforms;

static const uint INVALID_PAGE = 0xffffffffu;
static const uint VALID = 1u;
static const uint PENDING = 4u;

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 threadId : SV_DispatchThreadID)
{
    const uint requestIndex = threadId.x;
    const uint requestCount = min(compactedPageCount[0], uniforms.maxPageWork);
    if (requestIndex >= requestCount) return;

    const uint virtualPage = compactedPageKeys[requestIndex];
    const uint resident = virtualToPhysical[virtualPage];
    if (resident != INVALID_PAGE)
    {
        physicalMetadata[resident].lastUsedFrame = uniforms.frameIndex;
        return;
    }

    uint freeIndex = 0;
    uint observed = freePageCount[0];
    while (observed != 0u)
    {
        uint previous;
        InterlockedCompareExchange(freePageCount[0], observed, observed - 1u, previous);
        if (previous == observed)
        {
            freeIndex = observed;
            break;
        }
        observed = previous;
    }
    uint physicalPage = INVALID_PAGE;
    if (freeIndex != 0u && freeIndex <= uniforms.physicalPageCount)
        physicalPage = freePages[freeIndex - 1u];
    else
    {
        // Bounded approximate LRU.  The pool is only 256 pages, so a linear
        // scan is cheaper and more predictable than a reduction tree. Never
        // recycle a page still being produced this frame.
        uint oldestAge = 0u;
        [loop] for (uint candidate = 0u; candidate < uniforms.physicalPageCount; ++candidate)
        {
            const PhysicalPageMetadata candidateMetadata = physicalMetadata[candidate];
            if ((candidateMetadata.flags & PENDING) != 0u) continue;
            const uint age = uniforms.frameIndex - candidateMetadata.lastUsedFrame;
            if (age >= oldestAge && candidateMetadata.virtualPage != INVALID_PAGE)
            {
                oldestAge = age;
                physicalPage = candidate;
            }
        }
        if (physicalPage == INVALID_PAGE) return;
        const uint evictedVirtual = physicalToVirtual[physicalPage];
        if (evictedVirtual != INVALID_PAGE)
            virtualToPhysical[evictedVirtual] = INVALID_PAGE;
    }
    uint workIndex;
    InterlockedAdd(pageWorkCount[0], 1u, workIndex);
    if (workIndex >= uniforms.maxPageWork) return;

    const uint pagesPerLevel = uniforms.pageCountPerAxis * uniforms.pageCountPerAxis;
    const uint level = virtualPage / pagesPerLevel;
    const uint local = virtualPage - level * pagesPerLevel;
    const uint pageX = local % uniforms.pageCountPerAxis;
    const uint pageY = local / uniforms.pageCountPerAxis;
    float4x4 pageTransform = float4x4(1.0, 0.0, 0.0, 0.0,
                                      0.0, 1.0, 0.0, 0.0,
                                      0.0, 0.0, 1.0, 0.0,
                                      0.0, 0.0, 0.0, 1.0);
    pageTransform[0][0] = uniforms.pageCountPerAxis;
    pageTransform[1][1] = uniforms.pageCountPerAxis;
    pageTransform[3][0] = uniforms.pageCountPerAxis - 2.0 * pageX - 1.0;
    pageTransform[3][1] = uniforms.pageCountPerAxis - 2.0 * pageY - 1.0;
    PageWork work;
    work.viewProjection = mul(pageTransform, uniforms.clipMatrices[level]);
    work.drawSlot = workIndex;
    work.clipLevel = level;
    work.physicalPage = physicalPage;
    work.virtualPage = virtualPage;
    pageWork[workIndex] = work;
    physicalToVirtual[physicalPage] = virtualPage;
    PhysicalPageMetadata metadata;
    metadata.virtualPage = virtualPage;
    metadata.lastUsedFrame = uniforms.frameIndex;
    metadata.flags = PENDING;
    metadata.padding = 0;
    physicalMetadata[physicalPage] = metadata;
}
