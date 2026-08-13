#pragma once

#include "Engine/Math/Vec3.h"

#include <variant>

namespace Engine {
    struct BoxCollider {
        Vec3 halfExtents{0.5f, 0.5f, 0.5f};
    };

    struct SphereCollider {
        float radius = 0.5f;
    };

    struct CapsuleCollider {
        float radius = 0.5f;
        float height = 1.0f;
    };

    using ColliderShape = std::variant<BoxCollider, SphereCollider, CapsuleCollider>;

    struct ColliderComponent {
        ColliderShape shape = BoxCollider{};

        Vec3 offset{};
        bool isTrigger = false;
        float friction = 0.5f;
        float restitution = 0.0f;
    };
}