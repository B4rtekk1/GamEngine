// Per-meshlet refinement of the VSM page caster lists. gpu_culling admits a
// whole render section to a page when its bounds touch the page; a large
// section (a street, a facade) then rasterized every one of its triangles
// into every page it touched. For sections that have meshlets, gpu_culling
// instead lists the object here (and empties its section command). One
// row of workgroups per page (Y strides its object list, so a page with many
// listed objects is not walked serially by one group) tests each meshlet x
// instance sphere against the page. Visible meshlets that are neighbours in
// the expanded meshlet index buffer (which already holds absolute vertex
// indices) merge into one indexed draw, since the per-draw front-end cost
// dominates for meshlet-sized draws.
//
// Each object reserves its page budget with a single atomic after counting
// its runs. If at least half of it is visible, or the budget is exhausted,
// the object falls back to its original section command, so geometry is
// never dropped.
//
// SHADOW_MESH_TASKS=1 (shadow_meshlet_refine_mesh.spv) feeds the mesh-shader
// casters of Shadow/shadow_meshlet_mesh.slang instead: every visible meshlet
// becomes one entry (its meshlet id in indexCount, instanceCount 1, its
// transform in firstInstance) and the page counter counts entries. A mesh
// workgroup has no per-draw front-end cost, so nothing is merged, and an
// object leaves the meshlet stream only when the page budget runs out
// (Bistro: VSM raster 0.123 -> 0.099 ms against the half-visible rule).
import Culling.CullingTypes;

#ifndef SHADOW_MESH_TASKS
#define SHADOW_MESH_TASKS 0
#endif

struct GpuMeshlet { uint4 range; float4 bounds; float4 cone; uint4 material; };
struct ShadowInstanceTransform { float4 row0; float4 row1; float4 row2; };
struct ShadowPageWork
{
    float4x4 viewProjection;
    uint drawSlot;
    uint clipLevel;
    uint physicalPage;
    uint virtualPage;
};

[[vk::binding(0, 0)]] StructuredBuffer<CullingObjectData> objects;
[[vk::binding(1, 0)]] StructuredBuffer<GpuMeshlet> meshlets;
[[vk::binding(2, 0)]] StructuredBuffer<ShadowInstanceTransform> shadowTransforms;
[[vk::binding(3, 0)]] StructuredBuffer<ShadowPageWork> pageWork;
[[vk::binding(4, 0)]] StructuredBuffer<uint4> meshletObjects;
[[vk::binding(5, 0)]] StructuredBuffer<uint> meshletObjectCounts;
[[vk::binding(6, 0)]] RWStructuredBuffer<IndexedDrawCommand> meshletCommands;
[[vk::binding(7, 0)]] RWStructuredBuffer<uint> meshletCommandCounts;
[[vk::binding(8, 0)]] RWStructuredBuffer<IndexedDrawCommand> sectionCommands;

struct RefineSettings
{
    uint pageCount;
    // Capacity of one page's object list and section command slice.
    uint maxDrawCount;
    uint meshletCommandsPerPage;
    uint meshletCount;
};
[[vk::push_constant]] ConstantBuffer<RefineSettings> settings;

static const uint GroupSize = 128u;
// Must match ShadowPass::MeshletRefineObjectSlices.
static const uint ObjectSlices = 16u;

groupshared uint runsInObject;
groupshared uint trianglesInObject;
groupshared uint writtenInObject;
groupshared uint objectBase;
groupshared uint objectMode;
// Per chunk of GroupSize items: triangle range of each visible meshlet
// (begin == end when hidden), used to merge neighbours into one draw.
groupshared uint rangeBegin[GroupSize];
groupshared uint rangeEnd[GroupSize];

static const uint ModeMeshlets = 0u;
static const uint ModeSection = 1u;
static const uint ModeOverflow = 2u;

bool sphereOutsidePage(float3 center, float radius, float4x4 viewProjection)
{
    const float4 clip = mul(float4(center, 1.0), viewProjection);
    if (clip.w <= 0.00001) return false;
    // Orthographic clipmaps: each clip axis scales the world radius by the
    // length of its matrix column.
    const float3 r = radius * float3(
        length(float3(viewProjection[0].x, viewProjection[1].x, viewProjection[2].x)),
        length(float3(viewProjection[0].y, viewProjection[1].y, viewProjection[2].y)),
        length(float3(viewProjection[0].z, viewProjection[1].z, viewProjection[2].z)));
    return clip.x < -clip.w - r.x || clip.x > clip.w + r.x ||
           clip.y < -clip.w - r.y || clip.y > clip.w + r.y ||
           clip.z < -r.z || clip.z > clip.w + r.z;
}

// The one-sided stream draws with back-face culling, so a meshlet whose whole
// normal cone faces away from the light produces no depth. The clipmaps are
// orthographic: one light direction serves every point. `cone.w` is the
// cosine of the cone's half angle (below -1 when it has no cone).
bool lightFacing(float4 cone, ShadowInstanceTransform transform, float4x4 viewProjection)
{
    if (cone.w <= 0.0) return true;
    const float3 x = float3(transform.row0.x, transform.row1.x, transform.row2.x);
    const float3 y = float3(transform.row0.y, transform.row1.y, transform.row2.y);
    const float3 z = float3(transform.row0.z, transform.row1.z, transform.row2.z);
    // Cones are baked in mesh space: only a uniform, unmirrored scale keeps
    // their angle and the winding the rasterizer culls.
    const float3 lengths = float3(length(x), length(y), length(z));
    if (max(lengths.x, max(lengths.y, lengths.z)) > 1.001 * min(lengths.x, min(lengths.y, lengths.z)) ||
        dot(cross(x, y), z) <= 0.0)
        return true;
    const float3 axis = normalize(float3(dot(transform.row0.xyz, cone.xyz), dot(transform.row1.xyz, cone.xyz),
                                         dot(transform.row2.xyz, cone.xyz)));
    // Depth grows away from the light.
    const float3 toLight = -normalize(float3(viewProjection[0].z, viewProjection[1].z, viewProjection[2].z));
    const float sinCone = sqrt(saturate(1.0 - cone.w * cone.w));
    return dot(axis, toLight) > -sinCone - 0.02;
}

bool meshletVisible(CullingObjectData object, uint transformBase, uint item, float4x4 viewProjection,
                    out GpuMeshlet meshlet, out uint transformIndex)
{
    const uint meshletIndex = object.firstMeshlet + item % object.meshletCount;
    transformIndex = transformBase + item / object.meshletCount;
    meshlet = meshlets[min(meshletIndex, settings.meshletCount - 1u)];
    if (meshletIndex >= settings.meshletCount) return false;
    const ShadowInstanceTransform transform = shadowTransforms[transformIndex];
    const float4 local = float4(meshlet.bounds.xyz, 1.0);
    const float3 center = float3(dot(transform.row0, local), dot(transform.row1, local), dot(transform.row2, local));
    const float scale = max(length(float3(transform.row0.x, transform.row1.x, transform.row2.x)),
                            max(length(float3(transform.row0.y, transform.row1.y, transform.row2.y)),
                                length(float3(transform.row0.z, transform.row1.z, transform.row2.z))));
    if (sphereOutsidePage(center, meshlet.bounds.w * scale, viewProjection)) return false;
    // The two-sided stream rasterizes back faces as well.
    return object.twoSided != 0u || lightFacing(meshlet.cone, transform, viewProjection);
}

// Loads one chunk into rangeBegin/rangeEnd and returns whether this lane's
// item starts a run of visible meshlets that are contiguous in the shared
// index heap (ranges in indices; material.y is a meshlet's first index).
// Runs never cross an instance or a chunk.
bool loadChunk(CullingObjectData object, uint transformBase, uint chunkBase, uint items, uint lane,
               float4x4 viewProjection, out uint transformIndex)
{
    const uint item = chunkBase + lane;
    GpuMeshlet meshlet = (GpuMeshlet)0;
    transformIndex = 0u;
    const bool visible = item < items &&
                         meshletVisible(object, transformBase, item, viewProjection, meshlet, transformIndex);
    rangeBegin[lane] = visible ? meshlet.material.y : 0u;
    rangeEnd[lane] = visible ? meshlet.material.y + meshlet.range.w * 3u : 0u;
    GroupMemoryBarrierWithGroupSync();
#if SHADOW_MESH_TASKS
    return visible;
#else
    return visible && (lane == 0u || item % object.meshletCount == 0u ||
                       rangeEnd[lane - 1u] != rangeBegin[lane] || rangeBegin[lane - 1u] == rangeEnd[lane - 1u]);
#endif
}

// Last lane of the run starting at @p lane.
uint runEnd(CullingObjectData object, uint chunkBase, uint lane)
{
    uint last = lane;
    while (last + 1u < GroupSize && (chunkBase + last + 1u) % object.meshletCount != 0u &&
           rangeBegin[last + 1u] != rangeEnd[last + 1u] && rangeBegin[last + 1u] == rangeEnd[last])
        ++last;
    return last;
}

[numthreads(GroupSize, 1, 1)]
[shader("compute")]
void main(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    if (groupId.x >= settings.pageCount) return;
    const ShadowPageWork page = pageWork[groupId.x];
    const uint slot = page.drawSlot;
    const uint objectCount = min(meshletObjectCounts[slot], settings.maxDrawCount);
    const uint lane = threadId.x;

    for (uint listed = groupId.y; listed < objectCount; listed += ObjectSlices)
    {
        const uint4 entry = meshletObjects[slot * settings.maxDrawCount + listed];
        const CullingObjectData object = objects[entry.x];
        const uint items = object.meshletCount * object.instanceCount;
        if (lane == 0u)
        {
            runsInObject = 0u;
            trianglesInObject = 0u;
            writtenInObject = 0u;
        }
        GroupMemoryBarrierWithGroupSync();

        // Pass 1: count runs and visible triangles, so the page budget is
        // reserved in one atomic and the cheaper of the two draws is chosen.
        uint runs = 0u;
        uint triangles = 0u;
#if SHADOW_MESH_TASKS
        // Entries are single meshlets and no section/meshlet choice is made,
        // so this pass needs neither the shared ranges nor their barriers.
        // Each lane remembers its first 32 chunks for pass 2.
        uint visibleChunks = 0u;
        for (uint chunkBase = 0u, chunk = 0u; chunkBase < items; chunkBase += GroupSize, ++chunk)
        {
            GpuMeshlet meshlet;
            uint transformIndex;
            if (chunkBase + lane < items &&
                meshletVisible(object, entry.y, chunkBase + lane, page.viewProjection, meshlet, transformIndex))
            {
                ++runs;
                if (chunk < 32u) visibleChunks |= 1u << chunk;
            }
        }
#else
        for (uint chunkBase = 0u; chunkBase < items; chunkBase += GroupSize)
        {
            uint transformIndex;
            if (loadChunk(object, entry.y, chunkBase, items, lane, page.viewProjection, transformIndex))
                ++runs;
            triangles += (rangeEnd[lane] - rangeBegin[lane]) / 3u;
            GroupMemoryBarrierWithGroupSync();
        }
#endif
        InterlockedAdd(runsInObject, runs);
        InterlockedAdd(trianglesInObject, triangles);
        GroupMemoryBarrierWithGroupSync();

        if (lane == 0u)
        {
            // Every draw has a fixed front-end cost; once half of the object
            // is visible anyway the single section draw is cheaper.
            const uint objectTriangles = object.indexCount / 3u * object.instanceCount;
            uint base = 0u;
            uint mode = ModeMeshlets;
            if (runsInObject == 0u)
                mode = ModeMeshlets;
#if !SHADOW_MESH_TASKS
            // Mesh workgroups have no per-draw cost and cull per triangle, so
            // the mesh-task stream keeps every object on meshlets.
            else if (trianglesInObject * 2u >= objectTriangles)
                mode = ModeSection;
#endif
            else
            {
                InterlockedAdd(meshletCommandCounts[slot], runsInObject, base);
                if (base + runsInObject > settings.meshletCommandsPerPage)
                    mode = ModeOverflow;
            }
            objectBase = base;
            objectMode = mode;
            if (mode != ModeMeshlets)
                sectionCommands[slot * settings.maxDrawCount + entry.z] = IndexedDrawCommand(
                    object.indexCount, object.instanceCount, object.firstIndex, int(object.vertexOffset), entry.y);
        }
        GroupMemoryBarrierWithGroupSync();

        const uint pageCommands = slot * settings.meshletCommandsPerPage;
        if (objectMode == ModeMeshlets && runsInObject != 0u)
        {
            // Pass 2: the same chunks again; each run start writes one draw.
#if SHADOW_MESH_TASKS
            for (uint chunkBase = 0u, chunk = 0u; chunkBase < items; chunkBase += GroupSize, ++chunk)
            {
                const uint item = chunkBase + lane;
                bool visible = false;
                if (chunk < 32u)
                    visible = (visibleChunks >> chunk & 1u) != 0u;
                else if (item < items)
                {
                    GpuMeshlet meshlet;
                    uint unusedTransform;
                    visible = meshletVisible(object, entry.y, item, page.viewProjection, meshlet, unusedTransform);
                }
                if (visible)
                {
                    uint local;
                    InterlockedAdd(writtenInObject, 1u, local);
                    meshletCommands[pageCommands + objectBase + local] =
                        IndexedDrawCommand(object.firstMeshlet + item % object.meshletCount, 1u, 0u, 0,
                                           entry.y + item / object.meshletCount);
                }
            }
#else
            for (uint chunkBase = 0u; chunkBase < items; chunkBase += GroupSize)
            {
                uint transformIndex;
                if (loadChunk(object, entry.y, chunkBase, items, lane, page.viewProjection, transformIndex))
                {
                    uint local;
                    InterlockedAdd(writtenInObject, 1u, local);
#if SHADOW_MESH_TASKS
                    meshletCommands[pageCommands + objectBase + local] = IndexedDrawCommand(
                        object.firstMeshlet + (chunkBase + lane) % object.meshletCount, 1u, 0u, 0, transformIndex);
#else
                    const uint last = runEnd(object, chunkBase, lane);
                    meshletCommands[pageCommands + objectBase + local] = IndexedDrawCommand(
                        rangeEnd[last] - rangeBegin[lane], 1u, rangeBegin[lane], 0, transformIndex);
#endif
                }
                GroupMemoryBarrierWithGroupSync();
            }
#endif
        }
        else if (objectMode == ModeOverflow)
        {
            // The counter moved past this object's reservation; the part of
            // it below the budget is read by the draw and must be inert.
            const uint end = min(objectBase + runsInObject, settings.meshletCommandsPerPage);
            for (uint index = objectBase + lane; index < end; index += GroupSize)
                meshletCommands[pageCommands + index] = IndexedDrawCommand(0u, 0u, 0u, 0, 0u);
        }
        GroupMemoryBarrierWithGroupSync();
    }
}
