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
        Vec3 point;
        Vec3 normal;
        float distance{};
    };

    /**
     * @brief NVIDIA PhysX-backed rigid-body simulation and scene queries.
     */
    class PhysicsSystem final {
    public:
        explicit PhysicsSystem(Vec3 gravity = {0.0F, -9.81F, 0.0F}) noexcept //NOLINT g = 9.81
            : gravity_(gravity) {
        }

        /** Advances dynamic rigid bodies in a scene by one simulation step. */
        void update(Scene &scene, float deltaTime) const;

        /** Discards the PhysX world so the next update builds a fresh simulation. */
        void reset() const noexcept;

        [[nodiscard]] Vec3 gravity() const noexcept { return gravity_; }
        void setGravity(Vec3 gravity) noexcept { gravity_ = gravity; }

        [[nodiscard]] std::optional<RaycastHit> raycast(
            Scene &scene, Vec3 origin, Vec3 direction, float maxDistance = 1000.0F) const; //NOLINT

    private:
        struct BroadPhaseCache;

        Vec3 gravity_;
        mutable std::shared_ptr<BroadPhaseCache> broadPhaseCache_;
    };
} // namespace Engine
