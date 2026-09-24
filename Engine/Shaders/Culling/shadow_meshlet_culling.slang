// VSM fine culling is intentionally independent of meshlet_culling.slang.
// The latter consumes camera-frustum visibility and is therefore incorrect
// for a caster which is outside the camera but overlaps a shadow page.

struct GpuMeshlet { uint4 range; float4 bounds; float4 cone; uint4 material; };
struct GPUSceneInstanceRecord { float4x4 worldMatrix; float4 localBoundsMin; float4 localBoundsMax; uint4 idsAndFlags; float4 displacementBoundsPadding; };
struct GPUSceneMeshRecord { uint4 draw; uint4 lod; uint4 clusters; };
struct ShadowVisibleInstance { uint sourceInstanceId; uint shadowTransformIndex; };
struct ShadowVisibleMeshlet { uint pageSlot; uint sourceInstanceId; uint meshletId; };
struct ShadowPageWork { float4x4 viewProjection; uint drawSlot; uint clipLevel; uint physicalPage; uint virtualPage; };
struct ShadowMeshletCullUniforms { uint pageCount; uint instancesPerPage; uint meshletsPerPage; uint meshletCount; };

[[vk::binding(0, 0)]] StructuredBuffer<GpuMeshlet> meshlets;
[[vk::binding(1, 0)]] StructuredBuffer<GPUSceneInstanceRecord> instances;
[[vk::binding(2, 0)]] StructuredBuffer<GPUSceneMeshRecord> meshes;
// Page-local, compacted output of the coarse shadow caster cull.
[[vk::binding(3, 0)]] StructuredBuffer<ShadowVisibleInstance> visibleInstances;
[[vk::binding(4, 0)]] StructuredBuffer<uint> visibleInstanceCounts;
[[vk::binding(5, 0)]] StructuredBuffer<ShadowPageWork> pageWork;
[[vk::binding(6, 0)]] RWStructuredBuffer<ShadowVisibleMeshlet> visibleMeshlets;
[[vk::binding(7, 0)]] RWStructuredBuffer<uint> visibleMeshletTransformIndices;
[[vk::binding(8, 0)]] RWStructuredBuffer<uint> visibleMeshletCounts;
[[vk::binding(9, 0)]] ConstantBuffer<ShadowMeshletCullUniforms> culling;

bool sphereOutsidePage(float3 center, float radius, float4x4 viewProjection)
{
    const float4 clip = mul(float4(center, 1.0), viewProjection);
    // This conservative clip-space sphere bound is sufficient for the
    // orthographic VSM clipmaps. Keep near-plane crossings: rasterization
    // performs the exact clip and a false reject would lose a shadow.
    if (clip.w <= 0.00001) return false;
    const float r = radius;
    return clip.x < -clip.w - r || clip.x > clip.w + r ||
           clip.y < -clip.w - r || clip.y > clip.w + r ||
           clip.z < -r || clip.z > clip.w + r;
}

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    const uint pageSlot = groupId.y;
    const uint visibleInstanceIndex = groupId.x;
    if (pageSlot >= culling.pageCount || visibleInstanceIndex >= visibleInstanceCounts[pageSlot]) return;

    const uint sourceOffset = pageSlot * culling.instancesPerPage + visibleInstanceIndex;
    const ShadowVisibleInstance shadowInstance = visibleInstances[sourceOffset];
    const GPUSceneInstanceRecord instance = instances[shadowInstance.sourceInstanceId];
    const GPUSceneMeshRecord mesh = meshes[instance.idsAndFlags.x];
    const ShadowPageWork page = pageWork[pageSlot];
    const float3 axisX = mul(float4(1, 0, 0, 0), instance.worldMatrix).xyz;
    const float3 axisY = mul(float4(0, 1, 0, 0), instance.worldMatrix).xyz;
    const float3 axisZ = mul(float4(0, 0, 1, 0), instance.worldMatrix).xyz;
    const float maxScale = max(length(axisX), max(length(axisY), length(axisZ)));

    for (uint local = threadId.x; local < mesh.lod.z; local += 64u) {
        const uint meshletId = mesh.lod.y + local;
        if (meshletId >= culling.meshletCount) continue;
        const GpuMeshlet meshlet = meshlets[meshletId];
        const float3 center = mul(float4(meshlet.bounds.xyz, 1.0), instance.worldMatrix).xyz;
        const float radius = (meshlet.bounds.w + instance.displacementBoundsPadding.x) * maxScale;
        if (sphereOutsidePage(center, radius, page.viewProjection)) continue;

        uint destination;
        InterlockedAdd(visibleMeshletCounts[pageSlot], 1u, destination);
        if (destination >= culling.meshletsPerPage) continue;
        const uint output = pageSlot * culling.meshletsPerPage + destination;
        visibleMeshlets[output] = ShadowVisibleMeshlet(pageSlot, shadowInstance.sourceInstanceId, meshletId);
        // Kept in a parallel stream so ShadowVisibleMeshlet remains the
        // stable three-word ABI consumed by tooling and capture replay.
        visibleMeshletTransformIndices[output] = shadowInstance.shadowTransformIndex;
    }
}
