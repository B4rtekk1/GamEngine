// Fine-grained meshlet visibility pass. The renderer binds the output list as
// indirect mesh-task input once the VK_EXT_mesh_shader forward pipeline is active.

struct GpuMeshlet
{
    uint4 range;
    float4 bounds;
    float4 cone;
    uint4 material;
};

struct MeshletCullUniforms
{
    float4x4 viewProjection;
    float3 cameraPosition;
    uint meshletCount;
    float4 frustumPlanes[6];
};

[[vk::binding(0, 0)]] StructuredBuffer<GpuMeshlet> meshlets;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> visibleMeshlets;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> visibleCount;
[[vk::binding(3, 0)]] ConstantBuffer<MeshletCullUniforms> culling;

bool outsideFrustum(float3 center, float radius)
{
    for (uint plane = 0; plane < 6; ++plane)
        if (dot(culling.frustumPlanes[plane].xyz, center) + culling.frustumPlanes[plane].w < -radius)
            return true;
    return false;
}

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= culling.meshletCount) return;
    const GpuMeshlet meshlet = meshlets[id.x];
    if (outsideFrustum(meshlet.bounds.xyz, meshlet.bounds.w)) return;

    // The cone test is conservative for two-sided/degenerate meshlets.
    const float3 toCamera = normalize(culling.cameraPosition - meshlet.bounds.xyz);
    if (meshlet.cone.w >= -1.0)
    {
        // cone.w is cos(maximum angle between a primitive normal and axis).
        // A meshlet is fully back-facing only beyond the opposite cone; using
        // -sin(angle) preserves silhouettes rather than rejecting a flat
        // meshlet viewed from its front side (where cutoff is exactly 1).
        const float backfaceThreshold = -sqrt(max(0.0, 1.0 - meshlet.cone.w * meshlet.cone.w));
        if (dot(meshlet.cone.xyz, toCamera) <= backfaceThreshold) return;
    }

    uint destination;
    InterlockedAdd(visibleCount[0], 1, destination);
    if (destination < culling.meshletCount) visibleMeshlets[destination] = id.x;
}
