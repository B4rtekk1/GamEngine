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

void Scene::rebuildObjectHandles() {
    objects_.clear();
    names_.clear();
    registry_.view<>([this](const Entity entity) {
        std::string name = "Entity " + std::to_string(entityIndex(entity));
        if (registry_.has<NameComponent>(entity)) {
            name = registry_.get<NameComponent>(entity).value;
        }
        const ObjectId objectId = GameObject::nextObjectId();
        auto object = std::unique_ptr<GameObject>(
            new GameObject(registry_, entity, objectId, name));
        names_[name] = objectId;
        objects_.push_back(std::move(object));
    });
}

} // namespace Engine
