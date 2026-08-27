#pragma once

#include "Engine/ECS/Components/MeshRendererComponent.h"
#include "Engine/ECS/Components/TransformComponent.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/ECS/Components/ScriptComponent.h"
#include "Engine/ECS/Components/RigidbodyComponent.h"
#include "Engine/ECS/Components/ColliderComponent.h"
#include "Engine/ECS/Entity.h"
#include "Engine/ECS/Registry.h"
#include "Engine/Scene/Components/LightComponent.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace Engine {

class Actor;
class Scene;
class SceneEditor;
class ScenePreset;
class PhysicsSystem;
class Script;

/**
 * @brief Move-only owner for one ECS entity.
 *
 * This is intentionally not a polymorphic game-object base class. Gameplay
 * behavior belongs in systems and components; this helper only manages an
 * entity lifetime for code that needs RAII ownership.
 */
class GameObject final {
public:
    GameObject(const GameObject&) = delete;
    GameObject& operator=(const GameObject&) = delete;
    GameObject(GameObject&& other) noexcept
        : registry_(other.registry_), objectId_(std::exchange(other.objectId_, NullObjectId)),
          entity_(std::exchange(other.entity_, NullEntity)), name_(std::move(other.name_)) {}
    GameObject& operator=(GameObject&& other) noexcept {
        if (this != &other) {
            release();
            registry_ = other.registry_;
            objectId_ = std::exchange(other.objectId_, NullObjectId);
            entity_ = std::exchange(other.entity_, NullEntity);
            name_ = std::move(other.name_);
        }
        return *this;
    }
    ~GameObject() { release(); }

    [[nodiscard]] bool isSpawned() const noexcept {
        return entity_ != NullEntity && registry_ != nullptr && registry_->valid(entity_);
    }

    [[nodiscard]] TransformComponent& transform() { return get<TransformComponent>(); }
    [[nodiscard]] const TransformComponent& transform() const { return get<TransformComponent>(); }
    [[nodiscard]] MeshRendererComponent& meshRenderer() { return get<MeshRendererComponent>(); }
    [[nodiscard]] const MeshRendererComponent& meshRenderer() const { return get<MeshRendererComponent>(); }

    RigidbodyComponent& addRigidbody(RigidbodyComponent rigidbody = {}) {
        if (has<RigidbodyComponent>()) {
            registry_->modify<RigidbodyComponent>(entity_, [&](auto& value) { value = rigidbody; });
            return get<RigidbodyComponent>();
        }
        return add<RigidbodyComponent>(std::move(rigidbody));
    }
    [[nodiscard]] RigidbodyComponent& rigidbody() { return get<RigidbodyComponent>(); }
    [[nodiscard]] const RigidbodyComponent& rigidbody() const { return get<RigidbodyComponent>(); }

    void setTransform(TransformComponent transformValue) {
        transform() = std::move(transformValue);
        registry_->markChanged<TransformComponent>(entity_);
    }
    void setPosition(Vec3 value) { modifyTransform([&](auto& t) { t.position = value; }); }
    void setRotation(Vec3 value) { modifyTransform([&](auto& t) { t.rotation = value; }); }
    void setScale(Vec3 value) { modifyTransform([&](auto& t) { t.scale = value; }); }
    [[nodiscard]] const Vec3& position() const { return transform().position; }
    [[nodiscard]] const Vec3& rotation() const { return transform().rotation; }
    [[nodiscard]] const Vec3& scale() const { return transform().scale; }
    /** High-level mesh assignment; callers do not need to edit the component. */
    void setMesh(std::shared_ptr<const Mesh> mesh) {
        meshRenderer().mesh = std::move(mesh);
        registry_->markChanged<MeshRendererComponent>(entity_);
    }
    /** High-level material assignment; callers do not need to edit the component. */
    void setMaterial(PBRMaterial material) {
        meshRenderer().material = std::move(material);
        registry_->markChanged<MeshRendererComponent>(entity_);
    }
    void setCastShadow(bool enabled) {
        meshRenderer().castShadow = enabled;
        registry_->markChanged<MeshRendererComponent>(entity_);
    }
    void setCullingBatch(std::uint32_t batch) {
        meshRenderer().cullingBatch = batch;
        registry_->markChanged<MeshRendererComponent>(entity_);
    }

    /** Creates a triangle collider from this object's indexed mesh geometry. */
    ColliderComponent& addMeshCollider() {
        const auto& mesh = meshRenderer().mesh;
        if (mesh == nullptr || mesh->empty()) {
            throw std::logic_error("Cannot create a mesh collider without mesh vertices");
        }
        const ColliderComponent collider{.shape = MeshCollider{mesh}};
        if (has<ColliderComponent>()) {
            modify<ColliderComponent>([&](auto& value) { value = collider; });
        } else {
            add<ColliderComponent>(collider);
        }
        return get<ColliderComponent>();
    }

    CameraComponent& addCamera(CameraComponent camera = {}) {
        if (has<CameraComponent>()) {
            registry_->modify<CameraComponent>(entity_, [&](auto& value) { value = std::move(camera); });
            return get<CameraComponent>();
        }
        return add<CameraComponent>(std::move(camera));
    }
    [[nodiscard]] CameraComponent& camera() { return get<CameraComponent>(); }
    [[nodiscard]] const CameraComponent& camera() const { return get<CameraComponent>(); }
    LightComponent& addLight(LightComponent light = {}) {
        if (has<LightComponent>()) {
            registry_->modify<LightComponent>(entity_, [&](auto& value) { value = std::move(light); });
            return get<LightComponent>();
        }
        return add<LightComponent>(std::move(light));
    }
    [[nodiscard]] LightComponent& light() { return get<LightComponent>(); }
    [[nodiscard]] const LightComponent& light() const { return get<LightComponent>(); }

    ScriptComponent& addScript(std::string className, bool enabled = true) {
        if (has<ScriptComponent>()) {
            registry_->modify<ScriptComponent>(entity_, [&](auto& value) {
                value.className = std::move(className);
                value.enabled = enabled;
            });
            return get<ScriptComponent>();
        }
        return add<ScriptComponent>(ScriptComponent{std::move(className), enabled});
    }
    [[nodiscard]] bool isRenderable() const noexcept {
        return isSpawned() && meshRenderer().hasMesh();
    }
    [[nodiscard]] Mat4 modelMatrix() const noexcept { return transform().matrix(); }

private:
    friend class Actor;
    friend class Scene;
    friend class SceneEditor;
    friend class ScenePreset;
    friend class PhysicsSystem;
    friend class Script;

    explicit GameObject(Registry& registry, std::string_view name = {})
        : registry_(&registry), objectId_(nextObjectId()), name_(name) {}

    /** Creates the entity with the conventional transform and mesh components. */
    void spawn() {
        if (isSpawned()) return;
        entity_ = registry_->create();
        registry_->add<TransformComponent>(entity_);
        registry_->add<MeshRendererComponent>(entity_);
    }

    void destroy() noexcept { release(); }
    [[nodiscard]] Entity entity() const noexcept { return entity_; }
    [[nodiscard]] ObjectId objectId() const noexcept { return objectId_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void setName(std::string name) { name_ = std::move(name); }
    [[nodiscard]] Registry& registry() noexcept { return *registry_; }
    [[nodiscard]] const Registry& registry() const noexcept { return *registry_; }

    [[nodiscard]] GameObject clone() const {
        GameObject result(*registry_, name_);
        if (isSpawned()) result.entity_ = registry_->clone(entity_);
        return result;
    }

    template<typename T, typename... Args>
    T& add(Args&&... args) {
        requireSpawned();
        return registry_->add<T>(entity_, std::forward<Args>(args)...);
    }
    template<typename T>
    void remove() { requireSpawned(); registry_->remove<T>(entity_); }
    template<typename T, typename Func>
    void modify(Func&& func) {
        requireSpawned();
        registry_->modify<T>(entity_, std::forward<Func>(func));
    }
    template<typename T>
    [[nodiscard]] bool has() const { return isSpawned() && registry_->has<T>(entity_); }
    template<typename T>
    T& get() { requireSpawned(); return registry_->get<T>(entity_); }
    template<typename T>
    [[nodiscard]] const T& get() const { requireSpawned(); return registry_->get<T>(entity_); }

    GameObject(Registry& registry, const Entity entity, const ObjectId objectId,
               std::string name)
        : registry_(&registry), objectId_(objectId), entity_(entity), name_(std::move(name)) {}

    /** Detaches a scene-owned wrapper before its Registry is replaced. */
    void detach() noexcept { entity_ = NullEntity; }

    template<typename Func>
    void modifyTransform(Func&& func) {
        requireSpawned();
        registry_->modify<TransformComponent>(entity_, std::forward<Func>(func));
    }
    static ObjectId nextObjectId() noexcept { return nextObjectId_.fetch_add(1, std::memory_order_relaxed); }
    void requireSpawned() const {
        if (!isSpawned()) throw std::logic_error("GameObject does not own a live entity");
    }
    void release() noexcept {
        if (isSpawned()) registry_->destroy(entity_);
        entity_ = NullEntity;
    }

    inline static std::atomic<ObjectId> nextObjectId_{1};
    Registry* registry_{};
    ObjectId objectId_{NullObjectId};
    Entity entity_{NullEntity};
    std::string name_;
};

} // namespace Engine
