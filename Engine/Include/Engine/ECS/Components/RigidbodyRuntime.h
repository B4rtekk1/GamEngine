#pragma once

#include "Engine/Math/Vec3.h"

#include <optional>

namespace Engine {
    /** Runtime-only mirror of the last PhysX velocity. Never serialized. */
    struct RigidbodyState final {
        Vec3 linearVelocity;
        Vec3 angularVelocity;
    };

    /** Runtime-only input queue consumed once by PhysicsSystem. */
    struct PhysicsCommandBuffer final {
        Vec3 force;
        Vec3 torque;
        Vec3 impulse;
        Vec3 angularImpulse;
        std::optional<Vec3> linearVelocity;
        std::optional<Vec3> angularVelocity;
        std::optional<Vec3> teleportPosition;
        std::optional<Vec3> teleportRotation;

        void clear() noexcept { *this = {}; }
    };
} // namespace Engine
