// Generic page-table lookup shared by material shaders once a material is
// marked virtual.  A table entry is a physical cache-tile index; 0xffffffff
// means "not resident" and must fall back to a coarser mip.

static const uint VirtualTextureInvalidPage = 0xffffffffu;

struct VirtualTextureDesc
{
    uint tableOffset;
    uint mipOffset;
    uint virtualWidth;
    uint virtualHeight;
    uint pageSize;
    uint cacheTilesX;
    uint cacheTilesY;
    uint mipCount;
};

uint virtualTexturePageIndex(VirtualTextureDesc desc, StructuredBuffer<uint> mipOffsets,
                             uint mip, uint2 page, uint pagesX)
{
    return desc.tableOffset + mipOffsets[desc.mipOffset + mip] + page.y * pagesX + page.x;
}

float2 virtualTextureCacheUv(VirtualTextureDesc desc, uint physicalPage, float2 localUv)
{
    const uint2 tile = uint2(physicalPage % desc.cacheTilesX, physicalPage / desc.cacheTilesX);
    return (float2(tile) + saturate(localUv)) / float2(desc.cacheTilesX, desc.cacheTilesY);
}

// feedback[0] is an atomic counter. The remaining entries contain virtual
// page-table indices. It is intentionally lossy under pressure; CPU-side
// deduplication and the coarse-mip fallback make dropped requests harmless.
void virtualTextureRequestPage(RWStructuredBuffer<uint> feedback, uint feedbackCapacity, uint pageTableIndex)
{
    uint destination;
    InterlockedAdd(feedback[0], 1u, destination);
    if (destination < feedbackCapacity)
        feedback[1u + destination] = pageTableIndex;
}

// Resolves the finest available virtual page. `mipOffsets` is a per-texture
// table produced by VirtualTexture::mipOffsetsBuffer(). A missing fine page
// is emitted to feedback and the function walks toward a guaranteed coarser
// fallback; callers may sample their conventional texture when it returns
// VirtualTextureInvalidPage.
uint virtualTextureResolvePage(StructuredBuffer<uint> pageTable,
                               StructuredBuffer<uint> mipOffsets,
                               RWStructuredBuffer<uint> feedback,
                               uint feedbackCapacity, VirtualTextureDesc desc,
                               float2 uv, uint requestedMip, out uint resolvedMip)
{
    uint mip = min(requestedMip, desc.mipCount - 1u);
    [loop]
    for (;;)
    {
        uint2 extent = max(uint2(1u, 1u), uint2(desc.virtualWidth, desc.virtualHeight) >> mip);
        uint pagesX = (extent.x + desc.pageSize - 1u) / desc.pageSize;
        uint2 page = min(uint2(uv * float2(extent)) / desc.pageSize,
                         uint2(pagesX - 1u, (extent.y + desc.pageSize - 1u) / desc.pageSize - 1u));
        uint tableIndex = virtualTexturePageIndex(desc, mipOffsets, mip, page, pagesX);
        uint physical = pageTable[tableIndex];
        if (physical != VirtualTextureInvalidPage) { resolvedMip = mip; return physical; }
        virtualTextureRequestPage(feedback, feedbackCapacity, tableIndex);
        if (mip + 1u >= desc.mipCount) { resolvedMip = mip; return VirtualTextureInvalidPage; }
        ++mip;
    }
}
