// GPU VSM tile clear. One instance addresses one PageWork entry. The render
// pass uses an atlas-wide viewport; this vertex shader emits a quad covering
// only the selected physical tile and writes depth = 1.

struct PageWork
{
    float4x4 viewProjection;
    uint drawSlot;
    uint clipLevel;
    uint physicalPage;
    uint virtualPage;
};

[[vk::binding(0, 0)]] StructuredBuffer<PageWork> pageWork;

struct VertexOutput
{
    float4 position : SV_Position;
};

static const uint INVALID_PAGE = 0xffffffffu;
static const uint PAGE_RESOLUTION = 256u;
static const uint PHYSICAL_PAGES_PER_AXIS = 16u;
static const uint ATLAS_RESOLUTION = PAGE_RESOLUTION * PHYSICAL_PAGES_PER_AXIS;

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    const PageWork work = pageWork[instanceId];
    VertexOutput output;
    if (work.physicalPage == INVALID_PAGE)
    {
        output.position = float4(2.0, 2.0, 1.0, 1.0);
        return output;
    }

    const uint tileX = work.physicalPage % PHYSICAL_PAGES_PER_AXIS;
    const uint tileY = work.physicalPage / PHYSICAL_PAGES_PER_AXIS;
    const float2 minimum = float2(tileX * PAGE_RESOLUTION, tileY * PAGE_RESOLUTION) /
                           float(ATLAS_RESOLUTION) * 2.0 - 1.0;
    const float2 maximum = float2((tileX + 1u) * PAGE_RESOLUTION,
                                  (tileY + 1u) * PAGE_RESOLUTION) /
                           float(ATLAS_RESOLUTION) * 2.0 - 1.0;
    const float2 corners[6] = {
        float2(minimum.x, minimum.y), float2(maximum.x, minimum.y),
        float2(maximum.x, maximum.y), float2(minimum.x, minimum.y),
        float2(maximum.x, maximum.y), float2(minimum.x, maximum.y)};
    output.position = float4(corners[vertexId], 1.0, 1.0);
    return output;
}
