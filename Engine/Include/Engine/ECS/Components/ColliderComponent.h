#pragma once

/**
 * @file ColliderComponent.h
 * @brief Defines collider shapes and collision properties for scene entities.
 */

#include "Engine/Math/Vec3.h"

#include <variant>

namespace Engine {
    /**
     * @brief Box collider shape aligned with the owning transform's local axes.
     *
     * The extents are measured from the collider's center to each face.
     */
    struct BoxCollider {
        /// Half-width, half-height and half-depth in local units.
        Vec3 halfExtents{0.5F, 0.5F, 0.5F};
    };

    /**
     * @brief Spherical collider shape.
     */
    struct SphereCollider {
        /// Sphere radius in local units.
        float radius = 0.5F;
    };

    /**
     * @brief Capsule collider shape aligned with the component's local axis.
     */
    struct CapsuleCollider {
        /// Radius of the capsule's cylindrical section and hemispherical ends.
        float radius = 0.5F;

        /// Total capsule height in local units.
        float height = 1.0F;
    };

    /** @brief Collision shape for the unit ramp mesh (a sloped prism). */
    struct RampCollider {
        Vec3 halfExtents{0.5F, 0.5F, 0.5F};
    };

    /**
     * @brief Variant containing every collider shape supported by the engine.
     */
    using ColliderShape = std::variant<BoxCollider, SphereCollider, CapsuleCollider, RampCollider>;

    /**
     * @brief Physics collision properties attached to an entity.
     *
     * The component stores the shape, its local-space offset and material-like
     * response parameters used by the physics simulation.
     */
    struct ColliderComponent {
        /// Collision geometry used by this component.
        ColliderShape shape = BoxCollider{};

        /// Local-space offset of the collider relative to the entity transform.
        Vec3 offset{};

        /// Whether the collider reports overlaps without generating contacts.
        bool isTrigger = false;

        /// Friction coefficient used by the physics solver.
        float friction = 0.5F;

        /// Bounciness coefficient used by the physics solver.
        float restitution = 0.0F;
    };
}
