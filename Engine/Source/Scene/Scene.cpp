#include "Engine/Scene/Scene.h"

#include "Engine/Scene/SceneSerializer.h"

#include <utility>

namespace Engine {

GameObject& Scene::createMeshObject(std::string name,
                                    std::shared_ptr<const Mesh> mesh,
                                    PBRMaterial material) {
    GameObject& object = create(std::move(name));
    object.setMesh(std::move(mesh));
    object.setMaterial(std::move(material));
    return object;
}

void Scene::save(const std::filesystem::path& path) const {
    SceneSerializer::save(*this, path);
}

void Scene::load(const std::filesystem::path& path) {
    SceneSerializer::load(*this, path);
}

} // namespace Engine
