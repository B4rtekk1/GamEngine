#pragma once

#include "Engine/ECS/Registry.h"
#include "Engine/ECS/Actor.h"
#include "Engine/Math/Vec3.h"

#include <optional>
#include <memory>
#include <span>
#include <vector>

namespace Engine {
    class Scene;

    struct RaycastHit final {
        Actor actor;
        Vec3 point;
        Vec3 normal;
        float distance{};
    };

    /** Runtime-only shot diagnostics; positions remain valid after a target is destroyed. */
    struct BulletImpact final {
        Vec3 origin;
        Vec3 point;
        Vec3 normal;
        ObjectId target{NullObjectId};
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
            Scene &scene, Vec3 origin, Vec3 direction, float maxDistance = 1000.0F,
            ObjectId ignoredActor = NullObjectId) const; //NOLINT

    private:
        friend class GlassDestructionSystem;
        /** Read-only query after update has synchronized the PhysX world. */
        [[nodiscard]] std::optional<RaycastHit> raycastPrepared(
            Scene &scene, Vec3 origin, Vec3 direction, float maxDistance, ObjectId ignoredActor) const;
        struct BroadPhaseCache;

        Vec3 gravity_;
        mutable std::shared_ptr<BroadPhaseCache> broadPhaseCache_;
    };

    /**
     * Scene-bound gameplay facade over the engine's PhysX system.
     *
     * It intentionally omits the Scene argument: a Scene owns exactly one
     * facade and therefore exactly one underlying physics world.
     */
    class Physics final {
    public:
        explicit Physics(Scene &scene) noexcept : scene_(&scene) {
        }

        [[nodiscard]] Vec3 gravity() const noexcept { return system_.gravity(); }
        void setGravity(Vec3 gravity) noexcept { system_.setGravity(gravity); }

        [[nodiscard]] std::optional<RaycastHit> raycast(
            Vec3 origin, Vec3 direction, float maxDistance = 1000.0F) const; //NOLINT

        /** Fires a hitscan shot, damages opt-in glass and retains the most recent 128 surface impacts. */
        [[nodiscard]] std::optional<RaycastHit> shootRay(
            Vec3 origin, Vec3 direction, float maxDistance = 1000.0F,
            ObjectId ignoredActor = NullObjectId); //NOLINT

        /** Hitscan with explicit glass impact energy and world-space radius. */
        [[nodiscard]] std::optional<RaycastHit> shootRay(
            Vec3 origin, Vec3 direction, float maxDistance, ObjectId ignoredActor,
            float energy, float radius = 0.18F);

        [[nodiscard]] std::span<const BulletImpact> bulletImpacts() const noexcept { return bulletImpacts_; }
        void clearBulletImpacts() noexcept { bulletImpacts_.clear(); }

    private:
        friend class Application;
        friend class Scene;

        void update(float deltaTime) const;
        void reset() noexcept { system_.reset(); clearBulletImpacts(); }

        Scene *scene_;
        PhysicsSystem system_{};
        std::vector<BulletImpact> bulletImpacts_;
    };
} // namespace Engine
