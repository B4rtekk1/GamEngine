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
constexpr float RadiansToDegrees = 57.29577951308232f;
constexpr float SolidSphereInertiaFactor = 0.4f;
// Rolling resistance is much weaker than sliding Coulomb friction. This
// factor maps the collider friction coefficient to a practical rolling
// resistance coefficient for rigid spheres.
constexpr float RollingResistanceScale = 0.05f;

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

float rampTopAt(const Aabb& ramp, const Vec3& point, const float yawRadians) {
    const float deltaX = point.x() - ramp.center.x();
    const float deltaZ = point.z() - ramp.center.z();
    const float localZ = std::sin(yawRadians) * deltaX + std::cos(yawRadians) * deltaZ;
    return ramp.center.y() + localZ * ramp.extents.y() / ramp.extents.z();
}

Vec3 rampSurfaceNormal(const Aabb& ramp, const float yawRadians) {
    const float localZ = -ramp.extents.y() / ramp.extents.z();
    return Vec3{std::sin(yawRadians) * localZ, 1.0f,
                std::cos(yawRadians) * localZ}.normalized();
}

Vec3 rampLocalPosition(const Aabb& ramp, const Vec3& point, const float yawRadians) {
    const float deltaX = point.x() - ramp.center.x();
    const float deltaZ = point.z() - ramp.center.z();
    return {std::cos(yawRadians) * deltaX - std::sin(yawRadians) * deltaZ,
            point.y() - ramp.center.y(),
            std::sin(yawRadians) * deltaX + std::cos(yawRadians) * deltaZ};
}

Vec3 rampWorldDirection(const Vec3& localDirection, const float yawRadians) {
    return {std::cos(yawRadians) * localDirection.x() +
                std::sin(yawRadians) * localDirection.z(),
            localDirection.y(),
            -std::sin(yawRadians) * localDirection.x() +
                std::cos(yawRadians) * localDirection.z()};
}

struct SphereRampContact {
    Vec3 normal;
    float penetration;
};

std::optional<SphereRampContact> sphereRampContact(
    const Aabb& ramp, const Vec3& sphereCenter, const float radius,
    const Vec3& previousCenter, const float yawRadians) {
    if (radius <= 0.0f || ramp.extents.z() <= 0.0f) return std::nullopt;

    const Vec3 local = rampLocalPosition(ramp, sphereCenter, yawRadians);
    const Vec3 previousLocal = rampLocalPosition(ramp, previousCenter, yawRadians);
    const float slope = ramp.extents.y() / ramp.extents.z();

    // Ignore contacts entered through the solid underside. A small amount of
    // penetration is allowed so that discrete time steps do not lose a valid
    // contact with the sloped face.
    const float planeScale = std::sqrt(1.0f + slope * slope);
    const float previousPlaneDistance =
        (previousLocal.y() - slope * previousLocal.z()) / planeScale;
    if (previousPlaneDistance < -radius) return std::nullopt;

    const float closestX = std::clamp(local.x(), -ramp.extents.x(), ramp.extents.x());
    const float closestZ = std::clamp(
        (local.z() + slope * local.y()) / (1.0f + slope * slope),
        -ramp.extents.z(), ramp.extents.z());
    const Vec3 closestPoint{closestX, slope * closestZ, closestZ};
    const Vec3 separation = local - closestPoint;
    const float distance = separation.length();
    if (distance >= radius) return std::nullopt;

    const Vec3 localNormal = distance > 1e-6f
        ? separation * (1.0f / distance)
        : Vec3{0.0f, 1.0f, -slope}.normalized();
    if (localNormal.y() <= 0.0f) return std::nullopt;

    return SphereRampContact{
        rampWorldDirection(localNormal, yawRadians).normalized(),
        radius - distance
    };
}

float dot(const Vec3& lhs, const Vec3& rhs) {
    return lhs.x() * rhs.x() + lhs.y() * rhs.y() + lhs.z() * rhs.z();
}
} // namespace

void PhysicsSystem::update(Scene& scene, const float deltaTime) const {
    Registry& registry = scene.registry();
    const float dt = std::max(deltaTime, 0.0f);
    if (dt == 0.0f) return;

    struct StaticCollider { Entity entity; Aabb bounds; float yawRadians; };
    std::vector<StaticCollider> colliders;
    registry.view<ColliderComponent, Transform>(
        [&](const Entity entity, const ColliderComponent& collider, const Transform& transform) {
            if (!registry.has<RigidbodyComponent>(entity) ||
                registry.get<RigidbodyComponent>(entity).type == RigidbodyType::Static) {
                colliders.push_back({entity, worldAabb(transform, collider),
                    transform.rotation.y() * 0.01745329251994329577f});
            }
        });

    registry.view<RigidbodyComponent, Transform>(
        [&](const Entity entity, RigidbodyComponent& body, Transform& transform) {
            if (body.type != RigidbodyType::Dynamic) return;

            const Vec3 previousPosition = transform.position;
            if (body.useGravity) body.linearVelocity += gravity_ * dt;
            transform.position += body.linearVelocity * dt;

            bool touchesSurface = false;
            Vec3 surfaceNormal{0.0f, 1.0f, 0.0f};
            float contactFriction = 0.0f;
            if (registry.has<ColliderComponent>(entity)) {
                const auto& collider = registry.get<ColliderComponent>(entity);
                const bool isSphere = std::holds_alternative<SphereCollider>(collider.shape);
                Aabb bodyBounds = worldAabb(transform, collider);
                float contactHeight = -INFINITY;
                Vec3 contactNormal{0.0f, 1.0f, 0.0f};
                float selectedFriction = 0.0f;
                Vec3 rampCorrection{};
                Vec3 rampNormal{0.0f, 1.0f, 0.0f};
                float rampFriction = 0.0f;
                float rampPenetration = 0.0f;
                for (const StaticCollider& other : colliders) {
                    if (other.entity == entity || !overlapsHorizontally(bodyBounds, other.bounds)) continue;
                    const auto& otherCollider = registry.get<ColliderComponent>(other.entity);
                    const bool isRamp = std::holds_alternative<RampCollider>(otherCollider.shape);
                    if (isRamp && isSphere) {
                        const Vec3 previousCenter = previousPosition +
                            collider.offset * Vec3{std::abs(transform.scale.x()),
                                                   std::abs(transform.scale.y()),
                                                   std::abs(transform.scale.z())};
                        const auto contact = sphereRampContact(
                            other.bounds, bodyBounds.center, bodyBounds.extents.y(),
                            previousCenter, other.yawRadians);
                        if (contact && contact->penetration > rampPenetration) {
                            rampPenetration = contact->penetration;
                            rampCorrection = contact->normal * contact->penetration;
                            rampNormal = contact->normal;
                            rampFriction = std::sqrt(std::max(0.0f,
                                collider.friction * otherCollider.friction));
                        }
                        continue;
                    }
                    if (isRamp) {
                        const Vec3 localPosition = rampLocalPosition(other.bounds, bodyBounds.center,
                                                                     other.yawRadians);
                        if (std::abs(localPosition.x()) > other.bounds.extents.x() ||
                            std::abs(localPosition.z()) > other.bounds.extents.z()) {
                            continue;
                        }
                    }
                    const float otherTop = isRamp
                        ? rampTopAt(other.bounds, bodyBounds.center, other.yawRadians)
                        : other.bounds.center.y() + other.bounds.extents.y();
                    const Vec3 normal = isRamp ? rampSurfaceNormal(other.bounds, other.yawRadians)
                                               : Vec3{0.0f, 1.0f, 0.0f};
                    const float targetCenterY = isSphere
                        ? otherTop + bodyBounds.extents.y() / normal.y()
                        : otherTop + bodyBounds.extents.y();
                    const float previousTop = isRamp
                        ? rampTopAt(other.bounds, previousPosition, other.yawRadians)
                        : other.bounds.center.y() + other.bounds.extents.y();
                    const float previousTargetY = isSphere
                        ? previousTop + bodyBounds.extents.y() / normal.y()
                        : previousTop + bodyBounds.extents.y();
                    if (transform.position.y() <= targetCenterY + 1e-4f &&
                        previousPosition.y() >= previousTargetY - 1e-4f &&
                        dot(body.linearVelocity, normal) <= 1e-4f &&
                        targetCenterY > contactHeight) {
                        // Resolve only the highest eligible surface. Resolving
                        // each ramp immediately could let a later, lower
                        // contact pull the sphere through an adjacent ramp.
                        contactHeight = targetCenterY;
                        contactNormal = normal;
                        selectedFriction = std::sqrt(std::max(0.0f,
                            collider.friction * otherCollider.friction));
                    }
                }

                if (rampPenetration > 0.0f) {
                    transform.position += rampCorrection;
                    bodyBounds = worldAabb(transform, collider);
                    const float inwardVelocity = dot(body.linearVelocity, rampNormal);
                    if (inwardVelocity < 0.0f) {
                        body.linearVelocity -= rampNormal * inwardVelocity;
                    }
                    touchesSurface = true;
                    surfaceNormal = rampNormal;
                    contactFriction = rampFriction;
                }

                // A sphere can touch the ramp and the ground at the same
                // time near the ramp's lower edge. Resolve both contacts;
                // otherwise the ramp contact can mask the ground for one
                // frame and let the sphere tunnel below it.
                if (contactHeight > -INFINITY) {
                    if (transform.position.y() < contactHeight) {
                        transform.position.setY(contactHeight);
                        bodyBounds = worldAabb(transform, collider);
                    }
                    const float inwardVelocity = dot(body.linearVelocity, contactNormal);
                    if (inwardVelocity < 0.0f) {
                        body.linearVelocity -= contactNormal * inwardVelocity;
                    }
                    touchesSurface = true;
                    surfaceNormal = contactNormal;
                    contactFriction = selectedFriction;
                }

                if (touchesSurface && std::holds_alternative<SphereCollider>(collider.shape) &&
                    !body.fixedRotation) {
                    const float radius = bodyBounds.extents.y();
                    if (radius > 0.0f) {
                        const Vec3 gravityParallel = body.useGravity
                            ? gravity_ - surfaceNormal * dot(gravity_, surfaceNormal)
                            : Vec3{};
                        // A solid sphere rolls without slipping if static
                        // friction can provide the required tangential force.
                        // I = 2/5 mr^2, hence a = g_parallel / (1 + I/mr^2)
                        // = 5/7 g_parallel.
                        const float requiredFriction =
                            SolidSphereInertiaFactor / (1.0f + SolidSphereInertiaFactor) *
                            gravityParallel.length();
                        const float availableFriction = contactFriction *
                            std::abs(dot(gravity_, surfaceNormal));
                        const bool rollsWithoutSlipping = contactFriction > 1e-5f &&
                            availableFriction + 1e-5f >= requiredFriction;
                        if (rollsWithoutSlipping && body.useGravity) {
                            body.linearVelocity -= gravityParallel *
                                (SolidSphereInertiaFactor / (1.0f + SolidSphereInertiaFactor) * dt);

                            Vec3 tangentialVelocity = body.linearVelocity -
                                surfaceNormal * dot(body.linearVelocity, surfaceNormal);
                            const float tangentialSpeed = tangentialVelocity.length();
                            if (tangentialSpeed > 1e-6f) {
                                const float normalAcceleration =
                                    std::abs(dot(gravity_, surfaceNormal));
                                const float speedLoss = std::min(
                                    tangentialSpeed,
                                    contactFriction * RollingResistanceScale *
                                        normalAcceleration * dt);
                                body.linearVelocity -= tangentialVelocity *
                                    (speedLoss / tangentialSpeed);
                            }
                        }
                        if (rollsWithoutSlipping) {
                            const Vec3 tangentialVelocity = body.linearVelocity -
                                surfaceNormal * dot(body.linearVelocity, surfaceNormal);
                            body.angularVelocity = cross(surfaceNormal, tangentialVelocity) *
                                                   (RadiansToDegrees / radius);
                        }
                    }
                }
            }

            if (!body.fixedRotation) {
                transform.rotation += body.angularVelocity * dt;
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
