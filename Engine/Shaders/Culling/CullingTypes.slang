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
    uint lod1IndexCount;
    uint lod2IndexCount;
    // Squared world-space thresholds. Keeping these squared avoids a sqrt in
    // the per-object LOD hot path.
    float lod1DistanceSq;
    float lod2DistanceSq;
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
};

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
