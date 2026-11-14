// Mesh-shader VSM casters. shadow_meshlet_refine.slang (SHADOW_MESH_TASKS)
// lists, per page, the meshlets whose bounds touch the page; one workgroup
// rasterizes one of them into that page and rejects the triangles that
// cannot write depth before they reach the rasterizer. Only undisplaced
// sections are listed.
//
// SHADOW_MESH_ALPHA_TEST=0 (shadow_meshlet_mesh.spv): opaque one-sided
// casters; the vertices need no material and there is no fragment stage.
// SHADOW_MESH_ALPHA_TEST=1 (shadow_meshlet_mesh_alpha.spv): the two-sided
// foliage stream. Both faces are kept and fragmentMain applies the same
// cutout as Shadow/shadow_map.slang.
import Common.SceneData;
import Common.ShadowCasterData;

#ifndef SHADOW_MESH_ALPHA_TEST
#define SHADOW_MESH_ALPHA_TEST 0
#endif
#if SHADOW_MESH_ALPHA_TEST
import Common.Displacement; // materialTextures
[[vk::binding(2, 0)]] StructuredBuffer<MaterialData> materials;
#endif

static const uint MeshletMaxVertices = 64u;
static const uint MeshletMaxTriangles = 126u;
// One warp (NVIDIA's maxPreferredMeshWorkGroupInvocations); see
// Common/MeshletDraw.slang.
static const uint MeshletThreads = 32u;

struct MeshletRecord
{
    uint4 range; // vertexOffset, vertexCount, triangleOffset, triangleCount
    float4 bounds;
    float4 cone;
    uint4 material;
};

// GpuVertex, 32 bytes; only the position is read.
struct MeshletPackedVertex
{
    float3 position;
    uint tangentFrame;
    uint texCoord;
    uint texCoord1;
    uint color;
    uint materialIndex;
};

// The refine pass reuses its indexed-command buffer for the entries.
struct ShadowMeshletEntry
{
    uint meshletId;
    uint valid; // zero in the inert slots of an object that overflowed
    uint unused0;
    int unused1;
    uint transformIndex;
};

struct PushConstants
{
    float4x4 lightSpace;
    uint page;
    uint entriesPerPage;
    // Side of a page in texels.
    float pageResolution;
};

[[vk::binding(0, 1)]] StructuredBuffer<MeshletRecord> meshletRecords;
[[vk::binding(1, 1)]] StructuredBuffer<uint> meshletVertexRefs;
[[vk::binding(2, 1)]] StructuredBuffer<uint> meshletTriangleRefs;
[[vk::binding(3, 1)]] StructuredBuffer<MeshletPackedVertex> meshletVertexHeap;
[[vk::binding(4, 1)]] StructuredBuffer<ShadowMeshletEntry> meshletEntries;
[[vk::binding(5, 1)]] StructuredBuffer<uint> meshletEntryCounts;
[[vk::push_constant]] ConstantBuffer<PushConstants> pushConstants;

struct ShadowVertexOutput
{
    float4 position : SV_Position;
#if SHADOW_MESH_ALPHA_TEST
    [[vk::location(0)]] float2 texCoord;
    [[vk::location(1)]] nointerpolation uint materialIndex;
    [[vk::location(2)]] float2 texCoord1;
#endif
};
struct ShadowPrimitive { bool cull : SV_CullPrimitive; };
groupshared float4 meshletClip[MeshletMaxVertices];

// True when the triangle writes no depth. The one-sided pipeline uses
// counter-clockwise front faces with a positive-height viewport and culls
// back faces, so a visible face has a negative determinant (see
// Common/MeshletDraw.slang). Depth is left to the rasterizer's clipper.
bool triangleCulled(float4 a, float4 b, float4 c)
{
#if !SHADOW_MESH_ALPHA_TEST
    if (determinant(float3x3(a.xyw, b.xyw, c.xyw)) >= 0.0)
        return true;
#endif
    if ((a.x < -a.w && b.x < -b.w && c.x < -c.w) || (a.x > a.w && b.x > b.w && c.x > c.w) ||
        (a.y < -a.w && b.y < -b.w && c.y < -c.w) || (a.y > a.w && b.y > b.w && c.y > c.w))
        return true;
    if (min(a.w, min(b.w, c.w)) <= 0.0)
        return false;
    // No texel centre inside the bounds: nothing is rasterized. The guard
    // band covers the rasterizer's sub-pixel snapping.
    const float2 pa = (a.xy / a.w * 0.5 + 0.5) * pushConstants.pageResolution;
    const float2 pb = (b.xy / b.w * 0.5 + 0.5) * pushConstants.pageResolution;
    const float2 pc = (c.xy / c.w * 0.5 + 0.5) * pushConstants.pageResolution;
    const float2 lo = min(pa, min(pb, pc)) - (1.0 / 128.0);
    const float2 hi = max(pa, max(pb, pc)) + (1.0 / 128.0);
    return any(floor(lo + 0.5) == floor(hi + 0.5));
}

[numthreads(MeshletThreads, 1, 1)]
[outputtopology("triangle")]
[shader("mesh")]
void meshMain(uint3 groupId : SV_GroupID, uint threadId : SV_GroupThreadID,
              out vertices ShadowVertexOutput vertices[MeshletMaxVertices],
              out indices uint3 triangles[MeshletMaxTriangles],
              out primitives ShadowPrimitive primitives[MeshletMaxTriangles])
{
    ShadowMeshletEntry entry = (ShadowMeshletEntry)0;
    MeshletRecord meshlet = (MeshletRecord)0;
    if (groupId.x < min(meshletEntryCounts[pushConstants.page], pushConstants.entriesPerPage))
    {
        entry = meshletEntries[pushConstants.page * pushConstants.entriesPerPage + groupId.x];
        if (entry.valid != 0u)
            meshlet = meshletRecords[entry.meshletId];
    }
    SetMeshOutputCounts(meshlet.range.y, meshlet.range.w);
    // The transform is the same for every vertex: load it once per lane.
    uint materialOffset;
    const ShadowInstanceTransform transform = loadShadowCaster(entry.transformIndex, materialOffset);
    for (uint vertex = threadId; vertex < meshlet.range.y; vertex += MeshletThreads)
    {
        const MeshletPackedVertex packed = meshletVertexHeap[meshletVertexRefs[meshlet.range.x + vertex]];
        const float4 local = float4(packed.position, 1.0);
        const float3 world = float3(dot(transform.row0, local), dot(transform.row1, local),
                                    dot(transform.row2, local));
        const float4 clip = mul(float4(world, 1.0), pushConstants.lightSpace);
        vertices[vertex].position = clip;
#if SHADOW_MESH_ALPHA_TEST
        vertices[vertex].texCoord = float2(f16tof32(packed.texCoord & 0xffffu), f16tof32(packed.texCoord >> 16u));
        vertices[vertex].texCoord1 =
            float2(f16tof32(packed.texCoord1 & 0xffffu), f16tof32(packed.texCoord1 >> 16u));
        vertices[vertex].materialIndex = materialOffset + packed.materialIndex;
#endif
        meshletClip[vertex] = clip;
    }
    GroupMemoryBarrierWithGroupSync();
    // Not `triangle`; see Forward/depth_velocity.slang.
    for (uint tri = threadId; tri < meshlet.range.w; tri += MeshletThreads)
    {
        const uint packed = meshletTriangleRefs[meshlet.range.z + tri];
        const uint3 corners = uint3(packed & 0xffu, (packed >> 8u) & 0xffu, (packed >> 16u) & 0xffu);
        triangles[tri] = corners;
        primitives[tri].cull = triangleCulled(meshletClip[corners.x], meshletClip[corners.y], meshletClip[corners.z]);
    }
}

#if SHADOW_MESH_ALPHA_TEST
struct ShadowFragmentInput
{
    [[vk::location(0)]] float2 texCoord;
    [[vk::location(1)]] nointerpolation uint materialIndex;
    [[vk::location(2)]] float2 texCoord1;
};

// Same cutout policy as Shadow/shadow_map.slang.
[shader("fragment")]
void fragmentMain(ShadowFragmentInput input)
{
    MaterialData material = materials[input.materialIndex];
    const uint alphaMode = uint((material.textureIndices.w >> 1) & 3);
    if (alphaMode == 0)
        return;
    float alpha = material.roughnessAmbientOcclusion.w;
    if (material.textureIndices.x >= 0)
        alpha *= materialTextures[NonUniformResourceIndex(material.textureIndices.x)].Sample(
            materialTextureUv(material, 0, input.texCoord, input.texCoord1)).a;
    if (material.auxiliaryTextureIndices.y >= 0)
        alpha *= materialTextures[NonUniformResourceIndex(material.auxiliaryTextureIndices.y)].Sample(
            materialTextureUv(material, 4, input.texCoord, input.texCoord1)).r;
    const float cutoff = alphaMode == 1 ? material.roughnessAmbientOcclusion.z : 0.30;
    if ((alphaMode == 1 || alphaMode == 2) && alpha < cutoff)
        discard;
}
#endif
