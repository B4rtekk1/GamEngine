// Publishes only pages that have completed the shadow raster phase.
struct PageWork
{
    float4x4 viewProjection;
    uint drawSlot;
    uint clipLevel;
    uint physicalPage;
    uint virtualPage;
};

[[vk::binding(0, 0)]] StructuredBuffer<PageWork> pageWork;
[[vk::binding(1, 0)]] StructuredBuffer<uint> pageWorkCount;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> virtualToPhysical;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> physicalToVirtual;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint4> physicalMetadata;

static const uint INVALID_PAGE = 0xffffffffu;
static const uint VALID = 1u;
static const uint PENDING = 4u;

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 threadId : SV_DispatchThreadID)
{
    const uint index = threadId.x;
    if (index >= pageWorkCount[0]) return;
    const PageWork work = pageWork[index];
    if (work.physicalPage == INVALID_PAGE || work.virtualPage == INVALID_PAGE) return;

    // Allocation may have recycled a tile. Invalidate the old owner before
    // publishing the new owner, then clear PENDING and set VALID atomically
    // from the point of view of the following forward pass.
    const uint oldVirtualPage = physicalToVirtual[work.physicalPage];
    if (oldVirtualPage != INVALID_PAGE && oldVirtualPage != work.virtualPage)
        virtualToPhysical[oldVirtualPage] = INVALID_PAGE;
    virtualToPhysical[work.virtualPage] = work.physicalPage;
    physicalToVirtual[work.physicalPage] = work.virtualPage;
    uint4 metadata = physicalMetadata[work.physicalPage];
    metadata.x = work.virtualPage;
    metadata.z = VALID;
    physicalMetadata[work.physicalPage] = metadata;
}
