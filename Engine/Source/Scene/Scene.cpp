#include "Engine/Scene/Scene.h"

#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Assets/Content.h"
#include "Engine/Renderer/Geometry/Cube.h"

#include <cctype>
#include <cmath>
#include <stdexcept>
#include <utility>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>

// NOLINTBEGIN(readability-magic-numbers)

namespace Engine {
    namespace {
        [[nodiscard]] bool isGltfPath(const std::filesystem::path& path) {
            std::string extension = path.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](const unsigned char character) {
                               return static_cast<char>(std::tolower(character));
                           });
            return extension == ".gltf" || extension == ".glb";
        }

        [[nodiscard]] Transform localTransformFromMatrix(const glm::mat4 &matrix) {
            glm::vec3 scale{}, translation{}, skew{};
            glm::quat rotation{};
            glm::vec4 perspective{};
            if (!glm::decompose(matrix, scale, rotation, translation, skew, perspective)) {
                throw std::runtime_error("Cannot decompose transform while changing parent");
            }
            return Transform{
                .position = Vec3{translation},
                .rotation = Vec3{glm::degrees(glm::eulerAngles(glm::conjugate(glm::normalize(rotation))))},
                .scale = Vec3{scale},
            };
        }
    }

    GameObject &Scene::createMeshObject(std::string name,
                                        std::shared_ptr<const Mesh> mesh,
                                        PBRMaterial material) {
        GameObject &object = create(std::move(name));
        object.setMesh(std::move(mesh));
        object.setMaterial(material);
        return object;
    }

    Actor Scene::createMesh(std::string name, std::shared_ptr<const Mesh> mesh,
                            PBRMaterial material) {
        auto &object = createMeshObject(std::move(name), std::move(mesh), material);
        return Actor{*this, object.objectId()};
    }

    void Scene::setParent(const Actor &child, const Actor &parent, const ParentMode mode) {
        if (child.scene_ != this || parent.scene_ != this || !child.valid() || !parent.valid()) {
            throw std::invalid_argument("Parent and child must be live actors in this Scene");
        }
        if (child.objectId_ == parent.objectId_) {
            throw std::invalid_argument("An actor cannot be its own parent");
        }

        // Walking from the proposed parent to the root catches every cycle.
        for (Actor ancestor = parent; ancestor.valid(); ancestor = parentOf(ancestor)) {
            if (ancestor.objectId_ == child.objectId_) {
                throw std::invalid_argument("Parent relationship would create a cycle");
            }
        }

        const Entity childEntity = findEntity(child.objectId_);
        const Entity parentEntity = findEntity(parent.objectId_);
        Transform preservedLocal{};
        if (mode == ParentMode::KeepWorld) {
            TransformSystem::updateDirty(registry_);
            const glm::mat4 parentWorld = registry_.get<Transform>(parentEntity).worldMatrix().native();
            if (std::abs(glm::determinant(parentWorld)) < 1.0e-6F) {
                throw std::runtime_error("Cannot preserve world transform below a parent with a singular transform");
            }
            const glm::mat4 childWorld = registry_.get<Transform>(childEntity).worldMatrix().native();
            preservedLocal = localTransformFromMatrix(glm::inverse(parentWorld) * childWorld);
        }
        const UUID parentUuid = registry_.get<UUIDComponent>(parentEntity).value;
        if (registry_.has<ParentComponent>(childEntity)) {
            registry_.modify<ParentComponent>(childEntity, [parentUuid, parentEntity](ParentComponent &link) {
                link.parentUuid = parentUuid;
                link.runtimeParent = parentEntity;
            });
        } else {
            registry_.add<ParentComponent>(childEntity, ParentComponent{.parentUuid = parentUuid,
                                                                          .runtimeParent = parentEntity});
        }
        if (mode == ParentMode::KeepWorld) {
            registry_.modify<Transform>(childEntity, [&](Transform &transform) {
                transform.position = preservedLocal.position;
                transform.rotation = preservedLocal.rotation;
                transform.scale = preservedLocal.scale;
            });
        }
    }

    void Scene::clearParent(const Actor &child, const ParentMode mode) {
        if (child.scene_ != this || !child.valid()) {
            throw std::invalid_argument("Child must be a live actor in this Scene");
        }
        const Entity childEntity = findEntity(child.objectId_);
        if (registry_.has<ParentComponent>(childEntity)) {
            Transform preservedLocal{};
            if (mode == ParentMode::KeepWorld) {
                TransformSystem::updateDirty(registry_);
                preservedLocal = localTransformFromMatrix(
                    registry_.get<Transform>(childEntity).worldMatrix().native());
            }
            registry_.remove<ParentComponent>(childEntity);
            if (mode == ParentMode::KeepWorld) {
                registry_.modify<Transform>(childEntity, [&](Transform &transform) {
                    transform.position = preservedLocal.position;
                    transform.rotation = preservedLocal.rotation;
                    transform.scale = preservedLocal.scale;
                });
            }
        }
    }

    Actor Scene::parentOf(const Actor &child) const noexcept {
        if (child.scene_ != this || !child.valid()) return {};
        const Entity childEntity = findEntity(child.objectId_);
        if (!registry_.has<ParentComponent>(childEntity)) return {};
        const UUID parentUuid = registry_.get<ParentComponent>(childEntity).parentUuid;
        for (const auto &object : objects_) {
            const Entity entity = object->entity();
            if (registry_.has<UUIDComponent>(entity) &&
                registry_.get<UUIDComponent>(entity).value == parentUuid) {
                return Actor{const_cast<Scene &>(*this), object->objectId()};
            }
        }
        return {};
    }

    std::vector<Actor> Scene::childrenOf(const Actor &parent) const {
        std::vector<Actor> result;
        if (parent.scene_ != this || !parent.valid()) return result;
        const Entity parentEntity = findEntity(parent.objectId_);
        const UUID parentUuid = registry_.get<UUIDComponent>(parentEntity).value;
        for (const auto &object : objects_) {
            const Entity entity = object->entity();
            if (registry_.has<ParentComponent>(entity) &&
                registry_.get<ParentComponent>(entity).parentUuid == parentUuid) {
                result.push_back(Actor{const_cast<Scene &>(*this), object->objectId()});
            }
        }
        return result;
    }

    Actor Scene::createModel(std::string name, std::filesystem::path path,
                             const Assets::Content &content) {
        auto mesh = content.mesh(path);
        if (!mesh) {
            throw std::runtime_error("Could not load model for actor '" + name + "'");
        }
        auto &object = createMeshObject(std::move(name), std::move(mesh));
        Actor actor{*this, object.objectId()};
        if (isGltfPath(path)) actor.addMeshCollider();
        return actor;
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
                                                           material));
    }

    Actor Scene::createCube(std::string name, const PBRMaterial &material) {
        const auto prefab = Prefab::cube(material);
        return createPrefab(std::move(name), prefab);
    }

    Actor Scene::createPrefab(std::string name, const Prefab &prefab) {
        if (!prefab.mesh()) {
            throw std::invalid_argument("Cannot instantiate an empty prefab");
        }
        auto &object = createMeshObject(std::move(name), prefab.mesh(), prefab.material());
        object.setCastShadow(prefab.castShadow());
        object.setCullingBatch(prefab.cullingBatch());
        return Actor{*this, object.objectId()};
    }

    Actor Scene::createTerrain(std::string name, TerrainComponent terrain) {
        auto mesh = std::make_shared<Mesh>(terrain.createMesh());
        auto& object = createMeshObject(std::move(name), mesh, PBRMaterial{
            .baseColor = {0.74F, 0.78F, 0.70F},
            .metallic = 0.0F,
            .roughness = 0.92F,
            .terrainLayered = true,
        });
        object.addTerrain(std::move(terrain));
        object.addMeshCollider();
        return Actor{*this, object.objectId()};
    }

    void Scene::save(const std::filesystem::path &path) const {
        SceneSerializer::save(*this, path);
    }

    void Scene::load(const std::filesystem::path &path) {
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

// NOLINTEND(readability-magic-numbers)
