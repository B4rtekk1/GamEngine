#pragma once

#include "Engine/ECS/Components/MeshRendererComponent.h"
#include "Engine/ECS/Components/TransformComponent.h"
#include "Engine/ECS/Entity.h"
#include "Engine/ECS/Registry.h"

#include <atomic>
#include <stdexcept>
#include <utility>

namespace Engine {

/**
 * @brief Move-only owner for one ECS entity.
 *
 * This is intentionally not a polymorphic game-object base class. Gameplay
 * behavior belongs in systems and components; this helper only manages an
 * entity lifetime for code that needs RAII ownership.
 */
class GameObject final {
public:
    explicit GameObject(Registry& registry) noexcept
        : registry_(&registry), objectId_(nextObjectId()) {}

    GameObject(const GameObject&) = delete;
    GameObject& operator=(const GameObject&) = delete;
    GameObject(GameObject&& other) noexcept
        : registry_(other.registry_), objectId_(std::exchange(other.objectId_, NullObjectId)),
          entity_(std::exchange(other.entity_, NullEntity)) {}
    GameObject& operator=(GameObject&& other) noexcept {
        if (this != &other) {
            release();
            registry_ = other.registry_;
            objectId_ = std::exchange(other.objectId_, NullObjectId);
            entity_ = std::exchange(other.entity_, NullEntity);
        }
        return *this;
    }
    ~GameObject() { release(); }

    /** Creates the entity with the conventional transform and mesh components. */
    void spawn() {
        if (isSpawned()) return;
        entity_ = registry_->create();
        registry_->add<TransformComponent>(entity_);
        registry_->add<MeshRendererComponent>(entity_);
    }

    void destroy() noexcept { release(); }
    [[nodiscard]] bool isSpawned() const noexcept {
        return entity_ != NullEntity && registry_ != nullptr && registry_->valid(entity_);
    }
    [[nodiscard]] Entity entity() const noexcept { return entity_; }
    [[nodiscard]] ObjectId objectId() const noexcept { return objectId_; }
    [[nodiscard]] Registry& registry() noexcept { return *registry_; }
    [[nodiscard]] const Registry& registry() const noexcept { return *registry_; }

    [[nodiscard]] GameObject clone() const {
        GameObject result(*registry_);
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
    template<typename T>
    [[nodiscard]] bool has() const { return isSpawned() && registry_->has<T>(entity_); }
    template<typename T>
    T& get() { requireSpawned(); return registry_->get<T>(entity_); }
    template<typename T>
    [[nodiscard]] const T& get() const { requireSpawned(); return registry_->get<T>(entity_); }

    [[nodiscard]] TransformComponent& transform() { return get<TransformComponent>(); }
    [[nodiscard]] const TransformComponent& transform() const { return get<TransformComponent>(); }
    [[nodiscard]] MeshRendererComponent& meshRenderer() { return get<MeshRendererComponent>(); }
    [[nodiscard]] const MeshRendererComponent& meshRenderer() const { return get<MeshRendererComponent>(); }
    [[nodiscard]] Mat4 modelMatrix() const noexcept { return transform().matrix(); }

private:
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
};

} // namespace Engine
