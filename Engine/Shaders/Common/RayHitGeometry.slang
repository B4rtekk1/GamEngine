// Matches GpuVertex (36 bytes), GPUSceneInstanceRecord and GPUSceneMeshRecord.
// Global index entries already contain the global vertex offset.
struct RaySceneInstance {
    float4x4 worldMatrix;
    float4 localBoundsMin;
    float4 localBoundsMax;
    uint4 idsAndFlags;
    float4 displacementBoundsPadding;
};
struct RaySceneMesh {
    uint4 draw;
    uint4 lod;
    uint4 clusters;
};
struct RaySceneMaterial {
    uint4 data;
};
[[vk::binding(1, 1)]] StructuredBuffer<RaySceneInstance> rayInstances;
[[vk::binding(2, 1)]] StructuredBuffer<RaySceneMesh> rayMeshes;
[[vk::binding(3, 1)]] ByteAddressBuffer rayVertices;
[[vk::binding(4, 1)]] StructuredBuffer<uint> rayIndices;
[[vk::binding(8, 1)]] StructuredBuffer<RaySceneMaterial> rayMaterials;

struct RayVertex {
    float3 position;
    float3 normal;
    float2 uv0;
    float2 uv1;
    float3 color;
    uint materialIndex;
};

float3 decodeSnorm3x10(uint packed) {
    int3 signedValue = int3(int(packed << 22) >> 22,
                            int(packed << 12) >> 22,
                            int(packed << 2) >> 22);
    return clamp(float3(signedValue) / 511.0, -1.0, 1.0);
}

RayVertex loadRayVertex(uint index) {
    const uint byteOffset = index * 36u;
    RayVertex result;
    result.position = asfloat(rayVertices.Load3(byteOffset));
    result.normal = decodeSnorm3x10(rayVertices.Load(byteOffset + 12u));
    const uint uv0 = rayVertices.Load(byteOffset + 20u);
    const uint uv1 = rayVertices.Load(byteOffset + 24u);
    result.uv0 = float2(f16tof32(uv0 & 0xffffu), f16tof32(uv0 >> 16u));
    result.uv1 = float2(f16tof32(uv1 & 0xffffu), f16tof32(uv1 >> 16u));
    const uint color = rayVertices.Load(byteOffset + 28u);
    result.color = float3(color & 255u, (color >> 8u) & 255u,
                          (color >> 16u) & 255u) / 255.0;
    result.materialIndex = rayVertices.Load(byteOffset + 32u);
    return result;
}

struct RayHitSurface {
    float3 position;
    float3 normal;
    float2 uv0;
    float2 uv1;
    float3 color;
    uint materialIndex;
};

RayHitSurface loadRayHitSurface(uint instanceId, uint primitiveId, float2 barycentrics) {
    const RaySceneInstance instance = rayInstances[instanceId];
    const RaySceneMesh mesh = rayMeshes[instance.idsAndFlags.x];
    const uint triangle = mesh.draw.x + primitiveId * 3u;
    const RayVertex a = loadRayVertex(rayIndices[triangle]);
    const RayVertex b = loadRayVertex(rayIndices[triangle + 1u]);
    const RayVertex c = loadRayVertex(rayIndices[triangle + 2u]);
    const float3 weights = float3(1.0 - barycentrics.x - barycentrics.y,
                                  barycentrics.x, barycentrics.y);
    const float3 localPosition = a.position * weights.x + b.position * weights.y + c.position * weights.z;
    const float3 localNormal = normalize(a.normal * weights.x + b.normal * weights.y + c.normal * weights.z);
    const float3 axisX = mul(float4(1.0, 0.0, 0.0, 0.0), instance.worldMatrix).xyz;
    const float3 axisY = mul(float4(0.0, 1.0, 0.0, 0.0), instance.worldMatrix).xyz;
    const float3 axisZ = mul(float4(0.0, 0.0, 1.0, 0.0), instance.worldMatrix).xyz;
    const float determinant = dot(axisX, cross(axisY, axisZ));
    float3 worldNormal = localNormal.x * cross(axisY, axisZ) +
                         localNormal.y * cross(axisZ, axisX) +
                         localNormal.z * cross(axisX, axisY);
    worldNormal = normalize(worldNormal * (determinant < 0.0 ? -1.0 : 1.0));
    RayHitSurface surface;
    surface.position = mul(float4(localPosition, 1.0), instance.worldMatrix).xyz;
    surface.normal = worldNormal;
    surface.uv0 = a.uv0 * weights.x + b.uv0 * weights.y + c.uv0 * weights.z;
    surface.uv1 = a.uv1 * weights.x + b.uv1 * weights.y + c.uv1 * weights.z;
    surface.color = a.color * weights.x + b.color * weights.y + c.color * weights.z;
    surface.materialIndex = rayMaterials[instance.idsAndFlags.y].data.x + a.materialIndex;
    return surface;
}
