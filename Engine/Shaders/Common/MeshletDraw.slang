// Inputs of the mesh-shader forward path (descriptor set 1).
//
// meshlet_culling.slang produces the visible meshlets, meshlet_build_mesh_draws
// bins them per material program slot, and one mesh-shader workgroup then
// rasterizes one meshlet: it pulls the vertices from the geometry heap,
// shades them once per meshlet vertex and rejects triangles before they reach
// the rasterizer.
// Keep the records in sync with Culling::GpuMeshlet and Engine::GpuVertex.
module MeshletDraw;

public static const uint MeshletMaxVertices = 64u;
public static const uint MeshletMaxTriangles = 126u;
// Mesh workgroup size: maxPreferredMeshWorkGroupInvocations is 32 on NVIDIA
// (one warp, no cross-warp barrier); each lane then handles two vertices and
// up to four triangles of a 64/126 meshlet.
public static const uint MeshletThreads = 32u;
// Draws are laid out as rows of this many workgroups (maxMeshWorkGroupCount
// is 65535 per dimension); see meshlet_build_mesh_commands.slang.
public static const uint MeshletDrawRow = 32768u;
// MeshletDrawConstants::flags
public static const uint MeshletDrawBackfaceCull = 1u;
public static const uint MeshletDrawSmallPrimitiveCull = 2u;
// The visibility-buffer prepass writes this slot's triangle ids: its pixels
// are shaded by forward_pbr's visibilityResolveMain instead of rasterized.
public static const uint MeshletDrawVisibility = 4u;
// Material program slots (MaterialProgramSlotCount).
static const uint MeshletDrawSlotCount = 32u;

public struct MeshletRecord
{
    public uint4 range; // vertexOffset, vertexCount, triangleOffset, triangleCount
    public float4 bounds;
    public float4 cone;
    public uint4 material;
};

// GpuVertex, 32 bytes.
public struct MeshletPackedVertex
{
    public float3 position;
    public uint tangentFrame;
    public uint texCoord;  // two half floats
    public uint texCoord1; // two half floats
    public uint color;     // RGBA8 unorm
    public uint materialIndex;
};

public struct MeshletDrawConstants
{
    public uint slot;
    public uint flags;
};

// Set in MeshletDrawEntry::meshletId for foliage-pipeline and double-sided
// instances: their back faces are drawn. Global meshlet ids fit in 24 bits.
public static const uint MeshletDrawEntryTwoSided = 0x80000000u;

// x: index into RendererInstanceData (visibleInstances), y: global meshlet id
// plus MeshletDrawEntryTwoSided.
public struct MeshletDrawEntry
{
    public uint visibleInstance;
    public uint meshletId;
    public property uint meshlet { get { return meshletId & ~MeshletDrawEntryTwoSided; } }
    public property bool twoSided { get { return (meshletId & MeshletDrawEntryTwoSided) != 0u; } }
};

[[vk::binding(0, 1)]] public StructuredBuffer<MeshletRecord> meshletRecords;
[[vk::binding(1, 1)]] public StructuredBuffer<uint> meshletVertexRefs;
[[vk::binding(2, 1)]] public StructuredBuffer<uint> meshletTriangleRefs;
[[vk::binding(3, 1)]] public StructuredBuffer<MeshletPackedVertex> meshletVertexHeap;
[[vk::binding(4, 1)]] public StructuredBuffer<MeshletDrawEntry> meshletDrawList;
// 32 per-slot entry counts, then the packed slot offsets.
[[vk::binding(5, 1)]] public StructuredBuffer<uint> meshletDrawCounts;
[[vk::push_constant]] public ConstantBuffer<MeshletDrawConstants> meshletDraw;

// False for the padding workgroups of the last row. `listIndex` is the
// entry's position in meshletDrawList, which a visibility buffer stores.
public bool meshletDrawEntryFor(uint3 groupId, out MeshletDrawEntry entry, out uint listIndex)
{
    const uint start = meshletDrawCounts[MeshletDrawSlotCount + meshletDraw.slot];
    const uint capacity = meshletDrawCounts[MeshletDrawSlotCount + 1u + meshletDraw.slot] - start;
    const uint index = groupId.y * MeshletDrawRow + groupId.x;
    entry = (MeshletDrawEntry)0;
    listIndex = start + index;
    if (index >= min(meshletDrawCounts[meshletDraw.slot], capacity))
        return false;
    entry = meshletDrawList[start + index];
    return true;
}

public bool meshletDrawEntryFor(uint3 groupId, out MeshletDrawEntry entry)
{
    uint listIndex;
    return meshletDrawEntryFor(groupId, entry, listIndex);
}

// Visibility buffer texel: the meshletDrawList index above the triangle's
// 7-bit index within its meshlet; VisibilityNone where nothing was resolved.
public static const uint VisibilityTriangleBits = 7u;
public static const uint VisibilityNone = 0xffffffffu;

public float2 unpackMeshletHalf2(uint packed)
{
    return float2(f16tof32(packed & 0xffffu), f16tof32(packed >> 16u));
}

public float3 unpackMeshletColor(uint packed)
{
    return float3(float(packed & 0xffu), float((packed >> 8u) & 0xffu), float((packed >> 16u) & 0xffu)) *
           (1.0 / 255.0);
}

public uint3 unpackMeshletTriangle(uint packed)
{
    return uint3(packed & 0xffu, (packed >> 8u) & 0xffu, (packed >> 16u) & 0xffu);
}

// True when the triangle cannot produce a fragment. `viewport` is in pixels.
// The pipelines use counter-clockwise front faces with a positive-height
// viewport, so a front face has a negative signed area in NDC; the
// homogeneous determinant keeps that sign for triangles crossing w = 0.
// The mesh pipelines rasterize both faces: this is their only back-face
// test, so one-sided and two-sided meshlets share a draw.
public bool meshletTriangleCulled(float4 a, float4 b, float4 c, float2 viewport, bool twoSided)
{
    if (!twoSided && (meshletDraw.flags & MeshletDrawBackfaceCull) != 0u &&
        determinant(float3x3(a.xyw, b.xyw, c.xyw)) >= 0.0)
        return true;
    // Entirely outside one clip plane (the far plane is left to the rasterizer).
    if ((a.x < -a.w && b.x < -b.w && c.x < -c.w) || (a.x > a.w && b.x > b.w && c.x > c.w) ||
        (a.y < -a.w && b.y < -b.w && c.y < -c.w) || (a.y > a.w && b.y > b.w && c.y > c.w) ||
        (a.z < 0.0 && b.z < 0.0 && c.z < 0.0))
        return true;
    if ((meshletDraw.flags & MeshletDrawSmallPrimitiveCull) == 0u || min(a.w, min(b.w, c.w)) <= 0.0)
        return false;
    // No pixel centre inside the bounds: nothing is rasterized. The guard
    // band covers the rasterizer's sub-pixel snapping.
    const float2 pa = (a.xy / a.w * 0.5 + 0.5) * viewport;
    const float2 pb = (b.xy / b.w * 0.5 + 0.5) * viewport;
    const float2 pc = (c.xy / c.w * 0.5 + 0.5) * viewport;
    const float2 lo = min(pa, min(pb, pc)) - (1.0 / 128.0);
    const float2 hi = max(pa, max(pb, pc)) + (1.0 / 128.0);
    return any(floor(lo + 0.5) == floor(hi + 0.5));
}
