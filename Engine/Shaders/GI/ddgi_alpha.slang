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
