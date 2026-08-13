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

        float mass = 1.0f;
        float linearDamping = 0.05f;
        float angularDamping = 0.05f;

        bool useGravity = true;
        bool fixedRotation = false;

        Vec3 linearVelocity{};
        Vec3 angularVelocity{};

        std::uint64_t runtimeBody =0;
    };
}