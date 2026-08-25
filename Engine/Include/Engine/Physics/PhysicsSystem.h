#pragma once

#include "Engine/ECS/Registry.h"
#include "Engine/ECS/Actor.h"
#include "Engine/Math/Vec3.h"

#include <optional>
#include <memory>

namespace Engine {

class Scene;

struct RaycastHit final {
    Actor actor;
    Vec3 point{};
    Vec3 normal{};
    float distance{};
};

/**
 * @brief Advances dynamic rigidbodies and rolls grounded spheres.
 *
 * Dynamic sphere colliders derive their angular velocity from horizontal
 * movement while in contact with a collider surface.
 */
class PhysicsSystem final {
public:
    explicit PhysicsSystem(Vec3 gravity = {0.0f, -9.81f, 0.0f}) noexcept
        : gravity_(gravity) {}

    /** Advances dynamic rigid bodies in a scene by one simulation step. */
    void update(Scene& scene, float deltaTime) const;

    [[nodiscard]] Vec3 gravity() const noexcept { return gravity_; }
    void setGravity(Vec3 gravity) noexcept { gravity_ = gravity; }

    [[nodiscard]] std::optional<RaycastHit> raycast(
        Scene& scene, Vec3 origin, Vec3 direction, float maxDistance = 1000.0f) const;

private:
    struct BroadPhaseCache;

    Vec3 gravity_;
    mutable std::shared_ptr<BroadPhaseCache> broadPhaseCache_;
};

} // namespace Engine
