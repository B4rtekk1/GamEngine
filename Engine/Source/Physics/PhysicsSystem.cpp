#include "Engine/Physics/PhysicsSystem.h"

#include "Engine/Core/Transform.h"
#include "Engine/ECS/Components/ColliderComponent.h"
#include "Engine/ECS/Components/RigidbodyComponent.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <optional>
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
        } else if constexpr (std::is_same_v<Shape, RampCollider>) {
            return shape.halfExtents;
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

float rampTopAt(const Aabb& ramp, const float bodyZ) {
    const float localZ = bodyZ - ramp.center.z();
    return ramp.center.y() + localZ * ramp.extents.y() / ramp.extents.z();
}
} // namespace

void PhysicsSystem::update(Scene& scene, const float deltaTime) const {
    Registry& registry = scene.registry();
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
                    const auto& otherCollider = registry.get<ColliderComponent>(other.entity);
                    const float otherTop = std::holds_alternative<RampCollider>(otherCollider.shape)
                        ? rampTopAt(other.bounds, bodyBounds.center.z())
                        : other.bounds.center.y() + other.bounds.extents.y();
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

std::optional<RaycastHit> PhysicsSystem::raycast(
    Scene& scene, const Vec3 origin, Vec3 direction, const float maxDistance) const {
    const float directionLength = direction.length();
    if (directionLength <= 0.0f || maxDistance <= 0.0f) return std::nullopt;
    direction *= 1.0f / directionLength;

    std::optional<RaycastHit> result;
    float nearest = maxDistance;
    scene.registry().view<ColliderComponent, Transform>(
        [&](const Entity entity, const ColliderComponent& collider, const Transform& transform) {
            const Aabb bounds = worldAabb(transform, collider);
            float nearHit = 0.0f;
            float farHit = nearest;
            int hitAxis = -1;
            float hitSign = 0.0f;
            const float origins[3]{origin.x(), origin.y(), origin.z()};
            const float directions[3]{direction.x(), direction.y(), direction.z()};
            const float centers[3]{bounds.center.x(), bounds.center.y(), bounds.center.z()};
            const float extents[3]{bounds.extents.x(), bounds.extents.y(), bounds.extents.z()};

            for (int axis = 0; axis < 3; ++axis) {
                if (std::abs(directions[axis]) < 1e-6f) {
                    if (origins[axis] < centers[axis] - extents[axis] ||
                        origins[axis] > centers[axis] + extents[axis]) return;
                    continue;
                }
                const float inv = 1.0f / directions[axis];
                float t0 = (centers[axis] - extents[axis] - origins[axis]) * inv;
                float t1 = (centers[axis] + extents[axis] - origins[axis]) * inv;
                const float sign = directions[axis] > 0.0f ? -1.0f : 1.0f;
                if (t0 > t1) std::swap(t0, t1);
                if (t0 > nearHit) { nearHit = t0; hitAxis = axis; hitSign = sign; }
                farHit = std::min(farHit, t1);
                if (nearHit > farHit) return;
            }
            if (farHit < 0.0f || nearHit >= nearest) return;
            if (nearHit < 0.0f) nearHit = 0.0f;
            const Vec3 point = origin + direction * nearHit;
            Vec3 normal{};
            if (hitAxis == 0) normal = {hitSign, 0, 0};
            else if (hitAxis == 1) normal = {0, hitSign, 0};
            else if (hitAxis == 2) normal = {0, 0, hitSign};
            if (auto* object = scene.findByEntity(entity)) {
                nearest = nearHit;
                result = RaycastHit{Actor{scene, object->objectId()}, point, normal, nearHit};
            }
        });
    return result;
}

} // namespace Engine
