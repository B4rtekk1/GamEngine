#pragma once

#include "Engine/Math/Vec3.h"

namespace Engine {

    enum class RigidbodyType {
        Static,
        Dynamic,
        Kinematic
    };

    struct RigidbodyComponent {
        RigidbodyType type = RigidbodyType::Dynamic;

        float mass = 1.0F;
        float linearDamping = 0.05F;
        float angularDamping = 0.05F;

        bool useGravity = true;
        bool fixedRotation = false;

        Vec3 linearVelocity{};
        Vec3 angularVelocity{};

        // Forces and torques are accumulated until the next physics step.
        // They are intentionally not serialized: these are transient inputs.
        std::uint64_t runtimeBody = 0;
        Vec3 force{};
        Vec3 torque{};

        /** Adds a continuous force, applied during the next physics step. */
        void addForce(const Vec3& value) noexcept { force += value; }

        /** Adds a continuous torque, applied during the next physics step. */
        void addTorque(const Vec3& value) noexcept { torque += value; }

        /** Applies an instantaneous linear impulse to the body. */
        void addImpulse(const Vec3& impulse) noexcept {
            if (mass > 0.0F) linearVelocity += impulse * (1.0F / mass);
        }

        /** Applies an instantaneous angular impulse to the body. */
        void addAngularImpulse(const Vec3& impulse) noexcept {
            if (mass > 0.0F) angularVelocity += impulse * (1.0F / mass);
        }

        /** Removes all accumulated linear force. */
        void zeroForce() noexcept { force = {}; }

        /** Removes all accumulated torque. */
        void zeroTorque() noexcept { torque = {}; }

        /** Removes all accumulated forces and torques. */
        void zeroForces() noexcept {
            zeroForce();
            zeroTorque();
        }

        /** Stops linear and angular movement immediately. */
        void stop() noexcept {
            linearVelocity = {};
            angularVelocity = {};
        }

        [[nodiscard]] const Vec3& accumulatedForce() const noexcept { return force; }
        [[nodiscard]] const Vec3& accumulatedTorque() const noexcept { return torque; }
    };
}