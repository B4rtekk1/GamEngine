struct CullingObjectData
{
    float4x4 model;
    float4 localAabbMin;
    float4 localAabbMax;
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    uint vertexOffset;
    uint firstInstance;
    uint castShadow;
    uint twoSided;
    uint shader;
    uint shadowCacheMode;
    uint lod1IndexCount;
    uint lod2IndexCount;
    // Squared world-space thresholds. Keeping these squared avoids a sqrt in
    // the per-object LOD hot path.
    float lod1DistanceSq;
    float lod2DistanceSq;
    // Global meshlet range of the LOD0 indices; zero disables the per-meshlet
    // VSM path for this object (see shadow_meshlet_refine.slang).
    uint firstMeshlet;
    uint meshletCount;
};

struct IndexedDrawCommand
{
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};

struct CullingUniformData
{
    float4x4 viewProjection;
    float4x4 occlusionViewProjection;
    float4 cameraPosition;
    float4 frustumPlanes[6];
    uint objectCount;
    uint maxDrawCount;
    uint hiZMipCount;
    uint enableOcclusionCulling;
    float viewportWidth;
    float viewportHeight;
    float depthBias;
    float aabbExpansion;
    uint cameraCut;
    uint shadowPass;
    uint enableFrustumCulling;
    uint drawCategory;
    uint maxShadowInstances;
    uint shadowCacheMode;
    uint padding1;
    uint padding2;
    // Binned draws: slot s owns commands [binOffsets[s], binOffsets[s + 1]).
    uint4 binOffsets[9];
};

uint cullingBinOffset(CullingUniformData data, uint bin)
{
    return data.binOffsets[bin >> 2u][bin & 3u];
}

static const float3 UNIT_AABB_CORNERS[8] =
{
    float3(0.0, 0.0, 0.0),
    float3(1.0, 0.0, 0.0),
    float3(0.0, 1.0, 0.0),
    float3(1.0, 1.0, 0.0),
    float3(0.0, 0.0, 1.0),
    float3(1.0, 0.0, 1.0),
    float3(0.0, 1.0, 1.0),
    float3(1.0, 1.0, 1.0)
};
