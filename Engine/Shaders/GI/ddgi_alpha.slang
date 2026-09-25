import Common.SceneData;
import Common.Displacement;
import Common.RayHitGeometry;

[[vk::binding(2, 0)]] StructuredBuffer<MaterialData> materials;

bool ddgiHitIsDoubleSided(uint instanceId, uint primitiveId) {
    const RaySceneInstance instance = rayInstances[instanceId];
    const RaySceneMesh mesh = rayMeshes[instance.idsAndFlags.x];
    const uint firstVertex = rayIndices[mesh.draw.x + primitiveId * 3u];
    const uint materialIndex = rayMaterials[instance.idsAndFlags.y].data.x +
                               rayVertices.Load(firstVertex * 36u + 32u);
    return (materials[materialIndex].textureIndices.w & 1) != 0;
}

// Match the depth prepass cutout rule, including the foliage BLEND fallback.
bool ddgiCandidatePassesAlpha(uint instanceId, uint primitiveId, float2 barycentrics) {
    const RaySceneInstance instance = rayInstances[instanceId];
    const RaySceneMesh mesh = rayMeshes[instance.idsAndFlags.x];
    const uint triangle = mesh.draw.x + primitiveId * 3u;
    const RayVertex a = loadRayVertex(rayIndices[triangle]);
    const uint materialIndex = rayMaterials[instance.idsAndFlags.y].data.x + a.materialIndex;
    const MaterialData material = materials[materialIndex];
    const uint alphaMode = uint((material.textureIndices.w >> 1) & 3);
    if (alphaMode == 0u) return true;

    const RayVertex b = loadRayVertex(rayIndices[triangle + 1u]);
    const RayVertex c = loadRayVertex(rayIndices[triangle + 2u]);
    const float3 weights = float3(1.0 - barycentrics.x - barycentrics.y,
                                  barycentrics.x, barycentrics.y);
    const float2 uv0 = a.uv0 * weights.x + b.uv0 * weights.y + c.uv0 * weights.z;
    const float2 uv1 = a.uv1 * weights.x + b.uv1 * weights.y + c.uv1 * weights.z;
    float alpha = material.roughnessAmbientOcclusion.w;
    if (material.textureIndices.x >= 0)
        alpha *= materialTextures[NonUniformResourceIndex(material.textureIndices.x)]
            .SampleLevel(materialTextureUv(material, 0u, uv0, uv1), 0.0).a;
    if (material.auxiliaryTextureIndices.y >= 0)
        alpha *= materialTextures[NonUniformResourceIndex(material.auxiliaryTextureIndices.y)]
            .SampleLevel(materialTextureUv(material, 4u, uv0, uv1), 0.0).r;
    const float cutoff = alphaMode == 1u ? material.roughnessAmbientOcclusion.z : 0.30;
    return alpha >= cutoff;
}
