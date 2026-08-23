#pragma once

#include "Engine/ECS/Registry.h"
#include "Engine/Math/Vec3.h"

namespace Engine {

class Scene;

/**
 * @brief Applies the first, minimal part of rigidbody simulation.
 *
 * The system currently integrates gravity only. Collision detection,
 * constraints and angular motion are intentionally left for later.
 */
class PhysicsSystem final {
public:
    explicit PhysicsSystem(Vec3 gravity = {0.0f, -9.81f, 0.0f}) noexcept
        : gravity_(gravity) {}

    /** Advances dynamic rigid bodies by one simulation step. */
    void update(Registry& registry, float deltaTime) const;
    void update(Scene& scene, float deltaTime) const;

    [[nodiscard]] Vec3 gravity() const noexcept { return gravity_; }
    void setGravity(Vec3 gravity) noexcept { gravity_ = gravity; }

private:
    Vec3 gravity_;
};

} // namespace Engine
