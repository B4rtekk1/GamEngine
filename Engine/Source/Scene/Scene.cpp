#include "Engine/Scene/Scene.h"

#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Assets/Content.h"
#include "Engine/Renderer/Geometry/Cube.h"

#include <stdexcept>
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

Actor Scene::createModel(std::string name, std::filesystem::path path,
                         Assets::Content& content) {
    auto mesh = content.mesh(std::move(path));
    if (!mesh) throw std::runtime_error("Could not load model for actor '" + name + "'");
    auto& object = createMeshObject(std::move(name), std::move(mesh));
    return Actor{*this, object.objectId()};
}

Actor Scene::createModel(std::string name, std::filesystem::path path) {
    if (content_ == nullptr) {
        throw std::logic_error("Scene has no Content service attached");
    }
    return createModel(std::move(name), std::move(path), *content_);
}

Actor Scene::createPrefab(std::string name, std::filesystem::path path,
                          PBRMaterial material) {
    if (content_ == nullptr) {
        throw std::logic_error("Scene has no Content service attached");
    }
    return createPrefab(std::move(name), Prefab::model(*content_, std::move(path),
                                                       std::move(material)));
}

Actor Scene::createCube(std::string name, PBRMaterial material) {
    const auto prefab = Prefab::cube(std::move(material));
    return createPrefab(std::move(name), prefab);
}

Actor Scene::createPrefab(std::string name, const Prefab& prefab) {
    if (!prefab.mesh()) throw std::invalid_argument("Cannot instantiate an empty prefab");
    auto& object = createMeshObject(std::move(name), prefab.mesh(), prefab.material());
    object.setCastShadow(prefab.castShadow());
    object.setCullingBatch(prefab.cullingBatch());
    return Actor{*this, object.objectId()};
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
