#include <Engine/ECS/Components/MeshRendererComponent.h>
#include <Engine/Renderer/Geometry/Cube.h>

#include <limits>
#include <memory>

int main() {
    using namespace Engine;

    MeshRendererComponent renderer;
    if (renderer.hasMesh() || renderer.material.roughness != 0.55F ||
        !renderer.castShadow || renderer.cullingBatch != 0 ||
        renderer.occlusionQueryIndex != std::numeric_limits<std::uint32_t>::max()) return 1;

    renderer.mesh = std::make_shared<const Mesh>();
    if (renderer.hasMesh()) return 2;
    renderer.mesh = std::make_shared<const Mesh>(Cube::createMesh());
    if (!renderer.hasMesh()) return 3;

    renderer.material.baseColor = {0.1F, 0.2F, 0.3F, 0.4F};
    renderer.material.metallic = 0.9F;
    renderer.material.roughness = 0.15F;
    renderer.material.alphaBlend = true;
    renderer.castShadow = false;
    renderer.cullingBatch = 12;
    renderer.firstIndex = 24;
    renderer.occlusionQueryIndex = 3;
    if (renderer.material.baseColor.a() != 0.4F || renderer.material.metallic != 0.9F ||
        renderer.material.roughness != 0.15F || !renderer.material.alphaBlend ||
        renderer.castShadow || renderer.cullingBatch != 12 || renderer.firstIndex != 24 ||
        renderer.occlusionQueryIndex != 3) return 4;
    return 0;
}