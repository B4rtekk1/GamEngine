#include "Engine/Physics/PhysicsSystem.h"

#include "Engine/Core/Transform.h"
#include "Engine/ECS/Components/ColliderComponent.h"
#include "Engine/ECS/Components/RigidbodyComponent.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace Engine {

namespace {
struct Aabb {
    Vec3 center;
    Vec3 extents;
};

Vec3 colliderExtents(const ColliderComponent& collider) {
    return std::visit([](const auto& shape) {
        using Shape = std::decay_t<decltype(shape)>;
        if constexpr (std::is_same_v<Shape, BoxCollider>) {
            return shape.halfExtents;
        } else if constexpr (std::is_same_v<Shape, SphereCollider>) {
            return Vec3{shape.radius, shape.radius, shape.radius};
        } else {
            return Vec3{shape.radius, shape.height * 0.5f, shape.radius};
        }
    }, collider.shape);
}

Aabb worldAabb(const Transform& transform, const ColliderComponent& collider) {
    const Vec3 scale{std::abs(transform.scale.x()), std::abs(transform.scale.y()),
                     std::abs(transform.scale.z())};
    return {
        transform.position + collider.offset * scale,
        colliderExtents(collider) * scale
    };
}

bool overlapsHorizontally(const Aabb& lhs, const Aabb& rhs) {
    return std::abs(lhs.center.x() - rhs.center.x()) <= lhs.extents.x() + rhs.extents.x() &&
           std::abs(lhs.center.z() - rhs.center.z()) <= lhs.extents.z() + rhs.extents.z();
}
} // namespace

void PhysicsSystem::update(Scene& scene, const float deltaTime) const {
    update(scene.registry(), deltaTime);
}

void PhysicsSystem::update(Registry& registry, const float deltaTime) const {
    const float dt = std::max(deltaTime, 0.0f);
    if (dt == 0.0f) return;

    struct StaticCollider { Entity entity; Aabb bounds; };
    std::vector<StaticCollider> colliders;
    registry.view<ColliderComponent, Transform>(
        [&](const Entity entity, const ColliderComponent& collider, const Transform& transform) {
            if (!registry.has<RigidbodyComponent>(entity) ||
                registry.get<RigidbodyComponent>(entity).type == RigidbodyType::Static) {
                colliders.push_back({entity, worldAabb(transform, collider)});
            }
        });

    registry.view<RigidbodyComponent, Transform>(
        [&](const Entity entity, RigidbodyComponent& body, Transform& transform) {
            if (body.type != RigidbodyType::Dynamic || !body.useGravity) return;

            body.linearVelocity += gravity_ * dt;
            transform.position += body.linearVelocity * dt;

            if (registry.has<ColliderComponent>(entity)) {
                const auto& collider = registry.get<ColliderComponent>(entity);
                Aabb bodyBounds = worldAabb(transform, collider);
                for (const StaticCollider& other : colliders) {
                    if (other.entity == entity || !overlapsHorizontally(bodyBounds, other.bounds)) continue;
                    const float bodyBottom = bodyBounds.center.y() - bodyBounds.extents.y();
                    const float otherTop = other.bounds.center.y() + other.bounds.extents.y();
                    if (bodyBottom < otherTop && body.linearVelocity.y() <= 0.0f) {
                        transform.position.setY(transform.position.y() + otherTop - bodyBottom);
                        body.linearVelocity.setY(0.0f);
                        bodyBounds = worldAabb(transform, collider);
                    }
                }
            }
            registry.markChanged<Transform>(entity);
        });
}

} // namespace Engine
