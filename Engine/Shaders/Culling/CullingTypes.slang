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
    uint lod1IndexCount;
    uint lod2IndexCount;
    float lod1Distance;
    float lod2Distance;
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
    float4 cameraPosition;
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
