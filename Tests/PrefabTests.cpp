#include <Engine/Scene/Prefab.h>
#include <Engine/Scene/Scene.h>
#include <Engine/Assets/Content.h>

#include <filesystem>
#include <stdexcept>

int main() {
    using namespace Engine;

    PBRMaterial material{};
    material.baseColor = {0.2F, 0.4F, 0.6F, 1.0F};
    material.metallic = 0.8F;
    material.roughness = 0.3F;

    Prefab cube = Prefab::cube(material);
    if (!cube.mesh() || cube.mesh()->vertices.size() != 24 || cube.mesh()->indices.size() != 36 ||
        cube.material().metallic != 0.8F || cube.material().roughness != 0.3F ||
        !cube.castShadow() || cube.cullingBatch() != 0) return 1;
    cube.setCastShadow(false);
    cube.setCullingBatch(7);
    if (cube.castShadow() || cube.cullingBatch() != 7) return 2;

    Scene scene;
    const Actor actor = scene.createPrefab("PrefabCube", cube);
    const auto* object = scene.find("PrefabCube");
    if (!actor.valid() || !object || !object->isRenderable() ||
        object->meshRenderer().mesh != cube.mesh() || object->meshRenderer().castShadow ||
        object->meshRenderer().cullingBatch != 7 ||
        object->meshRenderer().material.baseColor.g() != 0.4F) return 3;

    const Actor convenience = scene.createCube("Convenience");
    if (!convenience.valid() || !scene.find("Convenience")->isRenderable() ||
        scene.find("Convenience")->meshRenderer().mesh->vertices.size() != 24) return 4;

    try {
        Assets::Content content;
        static_cast<void>(Prefab::model(content, "missing"));
        return 5;
    } catch (const std::runtime_error&) {
        return 0;
    }
}