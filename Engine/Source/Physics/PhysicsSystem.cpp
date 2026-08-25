#include "Engine/Physics/PhysicsSystem.h"

#include "Engine/Core/Transform.h"
#include "Engine/ECS/Components/ColliderComponent.h"
#include "Engine/ECS/Components/RigidbodyComponent.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Engine {

namespace {
constexpr float RadiansToDegrees = 57.29577951308232f;
constexpr float SolidSphereInertiaFactor = 0.4f;
// Rolling resistance is much weaker than sliding Coulomb friction. This
// factor maps the collider friction coefficient to a practical rolling
// resistance coefficient for rigid spheres.
constexpr float RollingResistanceScale = 0.05f;
// A perfectly inelastic side contact makes a rolling sphere appear glued to
// a wall. Keep a subtle rebound on near-vertical surfaces while allowing
// restitution-free floor contacts to remain stable.
constexpr float MinimumSphereWallRestitution = 0.15f;
constexpr float RestingBoxNormalSpeed = 0.2f;
constexpr float RestingBoxTangentialSpeed = 0.1f;
constexpr float RestingBoxAngularSpeedDegrees = 5.0f;

struct Aabb {
    Vec3 center;
    Vec3 extents;
};

struct SceneCollider {
    Entity entity;
    ColliderComponent collider;
    Transform transform;
    Aabb bounds;
    float yawRadians;
    bool dynamic;
};

constexpr float BroadPhaseCellSize = 4.0f;

struct Grid final {
    std::unordered_map<std::int64_t, std::vector<std::size_t>> cells;

    static std::int64_t key(const int x, const int z) noexcept {
        return static_cast<std::int64_t>(
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32u) |
            static_cast<std::uint32_t>(z));
    }

    static int coordinate(const float value) noexcept {
        return static_cast<int>(std::floor(value / BroadPhaseCellSize));
    }

    void insert(const Aabb& bounds, const std::size_t index) {
        const int minX = coordinate(bounds.center.x() - bounds.extents.x());
        const int maxX = coordinate(bounds.center.x() + bounds.extents.x());
        const int minZ = coordinate(bounds.center.z() - bounds.extents.z());
        const int maxZ = coordinate(bounds.center.z() + bounds.extents.z());
        for (int x = minX; x <= maxX; ++x) {
            for (int z = minZ; z <= maxZ; ++z) {
                cells[key(x, z)].push_back(index);
            }
        }
    }

    void query(const Aabb& bounds, std::vector<std::size_t>& result,
               std::vector<std::uint32_t>& seen, const std::uint32_t stamp) const {
        const int minX = coordinate(bounds.center.x() - bounds.extents.x());
        const int maxX = coordinate(bounds.center.x() + bounds.extents.x());
        const int minZ = coordinate(bounds.center.z() - bounds.extents.z());
        const int maxZ = coordinate(bounds.center.z() + bounds.extents.z());
        for (int x = minX; x <= maxX; ++x) {
            for (int z = minZ; z <= maxZ; ++z) {
                const auto it = cells.find(key(x, z));
                if (it == cells.end()) continue;
                for (const std::size_t index : it->second) {
                    if (seen[index] != stamp) {
                        seen[index] = stamp;
                        result.push_back(index);
                    }
                }
            }
        }
    }
};

float dot(const Vec3& lhs, const Vec3& rhs) {
    return lhs.x() * rhs.x() + lhs.y() * rhs.y() + lhs.z() * rhs.z();
}

struct OrientedBox {
    Vec3 center;
    Vec3 axisX;
    Vec3 axisY;
    Vec3 axisZ;
    Vec3 halfExtents;
};

Quat boxRotation(const Transform& transform) {
    const float toRadians = 1.0f / RadiansToDegrees;
    return Quat::angleAxis(transform.rotation.x() * toRadians, {1.0f, 0.0f, 0.0f}) *
           Quat::angleAxis(transform.rotation.y() * toRadians, {0.0f, 1.0f, 0.0f}) *
           Quat::angleAxis(transform.rotation.z() * toRadians, {0.0f, 0.0f, 1.0f});
}

OrientedBox orientedBox(const Transform& transform, const ColliderComponent& collider) {
    const Quat rotation = boxRotation(transform);
    const Vec3 scale{std::abs(transform.scale.x()), std::abs(transform.scale.y()),
                     std::abs(transform.scale.z())};
    return {
        transform.position + rotation * (collider.offset * scale),
        rotation * Vec3{1.0f, 0.0f, 0.0f},
        rotation * Vec3{0.0f, 1.0f, 0.0f},
        rotation * Vec3{0.0f, 0.0f, 1.0f},
        std::get<BoxCollider>(collider.shape).halfExtents * scale
    };
}

float boxSupportRadius(const OrientedBox& box, const Vec3& direction) {
    return std::abs(dot(box.axisX, direction)) * box.halfExtents.x() +
           std::abs(dot(box.axisY, direction)) * box.halfExtents.y() +
           std::abs(dot(box.axisZ, direction)) * box.halfExtents.z();
}

Vec3 boxSupportPoint(const OrientedBox& box, const Vec3& direction) {
    Vec3 result = box.center;
    const auto addAxis = [&](const Vec3& axis, const float extent) {
        const float projection = dot(axis, direction);
        // A zero projection means the whole edge/face is a support feature;
        // its centroid is the physically stable representative contact.
        if (std::abs(projection) > 1e-5f) {
            result += axis * (projection < 0.0f ? -extent : extent);
        }
    };
    addAxis(box.axisX, box.halfExtents.x());
    addAxis(box.axisY, box.halfExtents.y());
    addAxis(box.axisZ, box.halfExtents.z());
    return result;
}

Vec3 applyInverseBoxInertia(const OrientedBox& box, const float mass,
                            const Vec3& worldVector) {
    if (mass <= 0.0f) return {};
    const float ix = mass / 3.0f *
        (box.halfExtents.y() * box.halfExtents.y() +
         box.halfExtents.z() * box.halfExtents.z());
    const float iy = mass / 3.0f *
        (box.halfExtents.x() * box.halfExtents.x() +
         box.halfExtents.z() * box.halfExtents.z());
    const float iz = mass / 3.0f *
        (box.halfExtents.x() * box.halfExtents.x() +
         box.halfExtents.y() * box.halfExtents.y());
    return box.axisX * (dot(worldVector, box.axisX) / std::max(ix, 1e-6f)) +
           box.axisY * (dot(worldVector, box.axisY) / std::max(iy, 1e-6f)) +
           box.axisZ * (dot(worldVector, box.axisZ) / std::max(iz, 1e-6f));
}

void applyBoxSurfaceImpulse(RigidbodyComponent& body, const OrientedBox& box,
                            const Vec3& normal, const Vec3& contactPoint,
                            const float restitution, const float friction) {
    if (body.mass <= 0.0f) return;
    const float inverseMass = 1.0f / body.mass;
    Vec3 angularVelocityRadians = body.angularVelocity * (1.0f / RadiansToDegrees);
    const Vec3 lever = contactPoint - box.center;
    Vec3 contactVelocity = body.linearVelocity + cross(angularVelocityRadians, lever);
    const float normalSpeed = dot(contactVelocity, normal);

    float normalImpulseMagnitude = 0.0f;
    if (normalSpeed < 0.0f) {
        float angularTerm = 0.0f;
        if (!body.fixedRotation) {
            angularTerm = dot(normal, cross(
                applyInverseBoxInertia(box, body.mass, cross(lever, normal)), lever));
        }
        normalImpulseMagnitude = -(1.0f + restitution) * normalSpeed /
            std::max(inverseMass + angularTerm, 1e-6f);
        const Vec3 impulse = normal * normalImpulseMagnitude;
        body.linearVelocity += impulse * inverseMass;
        if (!body.fixedRotation) {
            angularVelocityRadians += applyInverseBoxInertia(
                box, body.mass, cross(lever, impulse));
        }
    }

    contactVelocity = body.linearVelocity + cross(angularVelocityRadians, lever);
    Vec3 tangent = contactVelocity - normal * dot(contactVelocity, normal);
    const float tangentSpeed = tangent.length();
    if (tangentSpeed > 1e-6f && friction > 0.0f && normalImpulseMagnitude > 0.0f) {
        tangent *= 1.0f / tangentSpeed;
        float angularTerm = 0.0f;
        if (!body.fixedRotation) {
            angularTerm = dot(tangent, cross(
                applyInverseBoxInertia(box, body.mass, cross(lever, tangent)), lever));
        }
        const float unconstrainedImpulse = -dot(contactVelocity, tangent) /
            std::max(inverseMass + angularTerm, 1e-6f);
        const float frictionLimit = friction * normalImpulseMagnitude;
        const float frictionImpulseMagnitude = std::clamp(
            unconstrainedImpulse, -frictionLimit, frictionLimit);
        const Vec3 impulse = tangent * frictionImpulseMagnitude;
        body.linearVelocity += impulse * inverseMass;
        if (!body.fixedRotation) {
            angularVelocityRadians += applyInverseBoxInertia(
                box, body.mass, cross(lever, impulse));
        }
    }

    if (!body.fixedRotation) {
        body.angularVelocity = angularVelocityRadians * RadiansToDegrees;
    }
}

void applySphereBoxImpulse(RigidbodyComponent& sphereBody,
                           RigidbodyComponent& boxBody,
                           const OrientedBox& box,
                           const Vec3& normal,
                           const Vec3& contactPoint,
                           const float restitution) {
    if (sphereBody.mass <= 0.0f || boxBody.mass <= 0.0f) return;

    const float sphereInverseMass = 1.0f / sphereBody.mass;
    const float boxInverseMass = 1.0f / boxBody.mass;
    const Vec3 lever = contactPoint - box.center;
    Vec3 boxAngularVelocityRadians =
        boxBody.angularVelocity * (1.0f / RadiansToDegrees);
    const Vec3 boxContactVelocity =
        boxBody.linearVelocity + cross(boxAngularVelocityRadians, lever);
    const float normalSpeed =
        dot(sphereBody.linearVelocity - boxContactVelocity, normal);
    if (normalSpeed >= 0.0f) return;

    float boxAngularTerm = 0.0f;
    if (!boxBody.fixedRotation) {
        boxAngularTerm = dot(normal, cross(
            applyInverseBoxInertia(box, boxBody.mass, cross(lever, normal)), lever));
    }
    const float impulseMagnitude = -(1.0f + restitution) * normalSpeed /
        std::max(sphereInverseMass + boxInverseMass + boxAngularTerm, 1e-6f);
    const Vec3 impulse = normal * impulseMagnitude;

    sphereBody.linearVelocity += impulse * sphereInverseMass;
    boxBody.linearVelocity -= impulse * boxInverseMass;
    if (!boxBody.fixedRotation) {
        boxAngularVelocityRadians -= applyInverseBoxInertia(
            box, boxBody.mass, cross(lever, impulse));
        boxBody.angularVelocity = boxAngularVelocityRadians * RadiansToDegrees;
    }
}

Vec3 colliderExtents(const ColliderComponent& collider) {
    return std::visit([]<typename T>(const T& shape) {
        using Shape = std::decay_t<T>;
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
    if (std::holds_alternative<BoxCollider>(collider.shape)) {
        const OrientedBox box = orientedBox(transform, collider);
        return {box.center, {
            std::abs(box.axisX.x()) * box.halfExtents.x() +
                std::abs(box.axisY.x()) * box.halfExtents.y() +
                std::abs(box.axisZ.x()) * box.halfExtents.z(),
            std::abs(box.axisX.y()) * box.halfExtents.x() +
                std::abs(box.axisY.y()) * box.halfExtents.y() +
                std::abs(box.axisZ.y()) * box.halfExtents.z(),
            std::abs(box.axisX.z()) * box.halfExtents.x() +
                std::abs(box.axisY.z()) * box.halfExtents.y() +
                std::abs(box.axisZ.z()) * box.halfExtents.z()}};
    }
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

struct BoxRampContact {
    Vec3 normal;
    float penetration;
    Vec3 point;
};

std::array<Vec3, 8> boxVertices(const OrientedBox& box) {
    std::array<Vec3, 8> vertices{};
    std::size_t index = 0;
    for (const float x : {-1.0f, 1.0f}) {
        for (const float y : {-1.0f, 1.0f}) {
            for (const float z : {-1.0f, 1.0f}) {
                vertices[index++] = box.center +
                    box.axisX * (x * box.halfExtents.x()) +
                    box.axisY * (y * box.halfExtents.y()) +
                    box.axisZ * (z * box.halfExtents.z());
            }
        }
    }
    return vertices;
}

std::array<Vec3, 6> rampVertices(const Aabb& ramp, const float yawRadians) {
    std::array<Vec3, 6> vertices{};
    std::size_t index = 0;
    for (const float x : {-ramp.extents.x(), ramp.extents.x()}) {
        for (const Vec3 yz : {
                 Vec3{x, -ramp.extents.y(), -ramp.extents.z()},
                 Vec3{x, -ramp.extents.y(), ramp.extents.z()},
                 Vec3{x, ramp.extents.y(), ramp.extents.z()}}) {
            vertices[index++] = ramp.center + rampWorldDirection(yz, yawRadians);
        }
    }
    return vertices;
}

template <std::size_t Count>
std::pair<float, float> projectedRange(
    const std::array<Vec3, Count>& vertices, const Vec3& axis) {
    float minimum = dot(vertices.front(), axis);
    float maximum = minimum;
    for (std::size_t index = 1; index < vertices.size(); ++index) {
        const float projection = dot(vertices[index], axis);
        minimum = std::min(minimum, projection);
        maximum = std::max(maximum, projection);
    }
    return {minimum, maximum};
}

std::optional<BoxRampContact> boxRampContact(
    const OrientedBox& box, const Aabb& ramp, const float yawRadians) {
    if (ramp.extents.x() <= 0.0f || ramp.extents.y() <= 0.0f ||
        ramp.extents.z() <= 0.0f) {
        return std::nullopt;
    }

    const auto boxPoints = boxVertices(box);
    const auto rampPoints = rampVertices(ramp, yawRadians);
    const float slope = ramp.extents.y() / ramp.extents.z();
    const std::array<Vec3, 3> boxAxes{box.axisX, box.axisY, box.axisZ};
    const std::array<Vec3, 4> rampFaceAxes{
        rampWorldDirection({1.0f, 0.0f, 0.0f}, yawRadians),
        rampWorldDirection({0.0f, 1.0f, 0.0f}, yawRadians),
        rampWorldDirection({0.0f, 0.0f, 1.0f}, yawRadians),
        rampWorldDirection({0.0f, 1.0f, -slope}, yawRadians).normalized()
    };
    const std::array<Vec3, 4> rampEdgeAxes{
        rampWorldDirection({1.0f, 0.0f, 0.0f}, yawRadians),
        rampWorldDirection({0.0f, 1.0f, 0.0f}, yawRadians),
        rampWorldDirection({0.0f, 0.0f, 1.0f}, yawRadians),
        rampWorldDirection({0.0f, slope, 1.0f}, yawRadians).normalized()
    };

    float smallestOverlap = INFINITY;
    Vec3 smallestAxis{};
    const auto testAxis = [&](Vec3 axis) {
        const float length = axis.length();
        if (length <= 1e-5f) return true;
        axis *= 1.0f / length;
        const auto [boxMinimum, boxMaximum] = projectedRange(boxPoints, axis);
        const auto [rampMinimum, rampMaximum] = projectedRange(rampPoints, axis);
        const float overlap = std::min(boxMaximum, rampMaximum) -
            std::max(boxMinimum, rampMinimum);
        if (overlap <= 0.0f) return false;
        if (overlap < smallestOverlap) {
            smallestOverlap = overlap;
            smallestAxis = axis;
        }
        return true;
    };

    for (const Vec3& axis : boxAxes) if (!testAxis(axis)) return std::nullopt;
    for (const Vec3& axis : rampFaceAxes) if (!testAxis(axis)) return std::nullopt;
    for (const Vec3& boxAxis : boxAxes) {
        for (const Vec3& rampEdgeAxis : rampEdgeAxes) {
            if (!testAxis(cross(boxAxis, rampEdgeAxis))) return std::nullopt;
        }
    }

    if (dot(smallestAxis, box.center - ramp.center) < 0.0f) {
        smallestAxis *= -1.0f;
    }
    return BoxRampContact{
        smallestAxis,
        smallestOverlap,
        boxSupportPoint(box, smallestAxis * -1.0f)
    };
}

struct SphereRampContact {
    Vec3 normal;
    float penetration;
};

using SphereContact = SphereRampContact;

std::optional<SphereContact> boxBoxContact(
    const Aabb& box, const Aabb& previousBox, const Aabb& other) {
    const Vec3 separation = box.center - other.center;
    const Vec3 overlap{
        box.extents.x() + other.extents.x() - std::abs(separation.x()),
        box.extents.y() + other.extents.y() - std::abs(separation.y()),
        box.extents.z() + other.extents.z() - std::abs(separation.z())};
    if (overlap.x() <= 0.0f || overlap.y() <= 0.0f || overlap.z() <= 0.0f) {
        return std::nullopt;
    }

    // Prefer the face crossed during this step. This prevents a fast-moving
    // box from being ejected through a neighbouring face after penetrating
    // more than one axis in a single frame.
    const Vec3 previousSeparation = previousBox.center - other.center;
    const float combined[3]{
        box.extents.x() + other.extents.x(),
        box.extents.y() + other.extents.y(),
        box.extents.z() + other.extents.z()};
    const float previous[3]{
        previousSeparation.x(), previousSeparation.y(), previousSeparation.z()};
    const float current[3]{separation.x(), separation.y(), separation.z()};
    const float penetrations[3]{overlap.x(), overlap.y(), overlap.z()};

    int selectedAxis = -1;
    float selectedPenetration = INFINITY;
    for (int axis = 0; axis < 3; ++axis) {
        const bool crossedFace = std::abs(previous[axis]) >= combined[axis] &&
            std::abs(current[axis]) < combined[axis];
        if (crossedFace && penetrations[axis] < selectedPenetration) {
            selectedAxis = axis;
            selectedPenetration = penetrations[axis];
        }
    }
    if (selectedAxis < 0) {
        selectedAxis = overlap.x() <= overlap.y() && overlap.x() <= overlap.z() ? 0
            : overlap.y() <= overlap.z() ? 1 : 2;
        selectedPenetration = penetrations[selectedAxis];
    }

    const float sign = current[selectedAxis] < 0.0f ? -1.0f : 1.0f;
    Vec3 normal{};
    if (selectedAxis == 0) normal = {sign, 0.0f, 0.0f};
    else if (selectedAxis == 1) normal = {0.0f, sign, 0.0f};
    else normal = {0.0f, 0.0f, sign};
    return SphereContact{normal, selectedPenetration};
}

std::optional<SphereContact> sphereBoxContact(
    const Vec3& sphereCenter, const float sphereRadius, const Aabb& box) {
    const Vec3 closest{
        std::clamp(sphereCenter.x(), box.center.x() - box.extents.x(),
                   box.center.x() + box.extents.x()),
        std::clamp(sphereCenter.y(), box.center.y() - box.extents.y(),
                   box.center.y() + box.extents.y()),
        std::clamp(sphereCenter.z(), box.center.z() - box.extents.z(),
                   box.center.z() + box.extents.z())};
    const Vec3 separation = sphereCenter - closest;
    const float distance = separation.length();
    if (distance > 1e-6f) {
        if (distance >= sphereRadius) return std::nullopt;
        return SphereContact{separation * (1.0f / distance), sphereRadius - distance};
    }

    // The sphere center is inside the box. Push it through the nearest face;
    // using only the closest-point test would leave it trapped there.
    const Vec3 local = sphereCenter - box.center;
    const float xDistance = box.extents.x() - std::abs(local.x());
    const float yDistance = box.extents.y() - std::abs(local.y());
    const float zDistance = box.extents.z() - std::abs(local.z());
    if (xDistance <= yDistance && xDistance <= zDistance) {
        return SphereContact{{local.x() < 0.0f ? -1.0f : 1.0f, 0.0f, 0.0f},
                             sphereRadius + xDistance};
    }
    if (yDistance <= zDistance) {
        return SphereContact{{0.0f, local.y() < 0.0f ? -1.0f : 1.0f, 0.0f},
                             sphereRadius + yDistance};
    }
    return SphereContact{{0.0f, 0.0f, local.z() < 0.0f ? -1.0f : 1.0f},
                         sphereRadius + zDistance};
}

std::optional<SphereContact> sphereOrientedBoxContact(
    const Vec3& sphereCenter, const float sphereRadius, const OrientedBox& box) {
    const Vec3 fromBox = sphereCenter - box.center;
    const Vec3 local{
        dot(fromBox, box.axisX),
        dot(fromBox, box.axisY),
        dot(fromBox, box.axisZ)
    };
    const Vec3 closestLocal{
        std::clamp(local.x(), -box.halfExtents.x(), box.halfExtents.x()),
        std::clamp(local.y(), -box.halfExtents.y(), box.halfExtents.y()),
        std::clamp(local.z(), -box.halfExtents.z(), box.halfExtents.z())
    };
    const Vec3 closest = box.center +
        box.axisX * closestLocal.x() +
        box.axisY * closestLocal.y() +
        box.axisZ * closestLocal.z();
    const Vec3 separation = sphereCenter - closest;
    const float distance = separation.length();
    if (distance > 1e-6f) {
        if (distance >= sphereRadius) return std::nullopt;
        return SphereContact{separation * (1.0f / distance), sphereRadius - distance};
    }

    const float xDistance = box.halfExtents.x() - std::abs(local.x());
    const float yDistance = box.halfExtents.y() - std::abs(local.y());
    const float zDistance = box.halfExtents.z() - std::abs(local.z());
    if (xDistance <= yDistance && xDistance <= zDistance) {
        return SphereContact{box.axisX * (local.x() < 0.0f ? -1.0f : 1.0f),
                             sphereRadius + xDistance};
    }
    if (yDistance <= zDistance) {
        return SphereContact{box.axisY * (local.y() < 0.0f ? -1.0f : 1.0f),
                             sphereRadius + yDistance};
    }
    return SphereContact{box.axisZ * (local.z() < 0.0f ? -1.0f : 1.0f),
                         sphereRadius + zDistance};
}

std::optional<SphereContact> sphereSphereContact(
    const Vec3& center, const float radius, const Aabb& other) {
    const float otherRadius = std::max({other.extents.x(), other.extents.y(), other.extents.z()});
    const Vec3 separation = center - other.center;
    const float distance = separation.length();
    const float combinedRadius = radius + otherRadius;
    if (distance >= combinedRadius) return std::nullopt;
    const Vec3 normal = distance > 1e-6f
        ? separation * (1.0f / distance)
        : Vec3{0.0f, 1.0f, 0.0f};
    return SphereContact{normal, combinedRadius - distance};
}

std::optional<SphereContact> sphereCapsuleContact(
    const Vec3& center, const float radius, const Aabb& capsule) {
    const float capsuleRadius = std::max(capsule.extents.x(), capsule.extents.z());
    const float segmentHalfHeight = std::max(0.0f, capsule.extents.y() - capsuleRadius);
    const Vec3 closest{capsule.center.x(),
        std::clamp(center.y(), capsule.center.y() - segmentHalfHeight,
                   capsule.center.y() + segmentHalfHeight),
        capsule.center.z()};
    const Vec3 separation = center - closest;
    const float distance = separation.length();
    const float combinedRadius = radius + capsuleRadius;
    if (distance >= combinedRadius) return std::nullopt;
    const Vec3 normal = distance > 1e-6f
        ? separation * (1.0f / distance)
        : Vec3{0.0f, 1.0f, 0.0f};
    return SphereContact{normal, combinedRadius - distance};
}

std::optional<SphereContact> sphereColliderContact(
    const Vec3& center, const float radius, const Aabb& other,
    const ColliderComponent& otherCollider) {
    return std::visit([&]<typename T>([[maybe_unused]] const T& shape) -> std::optional<SphereContact> {
        using Shape = std::decay_t<T>;
        if constexpr (std::is_same_v<Shape, BoxCollider>) {
            return sphereBoxContact(center, radius, other);
        } else if constexpr (std::is_same_v<Shape, SphereCollider>) {
            return sphereSphereContact(center, radius, other);
        } else if constexpr (std::is_same_v<Shape, CapsuleCollider>) {
            return sphereCapsuleContact(center, radius, other);
        } else {
            return std::nullopt;
        }
    }, otherCollider.shape);
}

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

} // namespace

struct PhysicsSystem::BroadPhaseCache {
    const Registry* registry = nullptr;
    std::uint64_t structuralRevision = 0;
    std::uint64_t colliderRevision = 0;
    std::uint64_t transformRevision = 0;
    std::uint64_t rigidbodyRevision = 0;
    std::vector<SceneCollider> staticColliders;
};

void PhysicsSystem::update(Scene& scene, const float deltaTime) const {
    Registry& registry = scene.registry();
    const float dt = std::max(deltaTime, 0.0f);
    if (dt == 0.0f) return;

    std::vector<SceneCollider> colliders;

    if (!broadPhaseCache_) broadPhaseCache_ = std::make_shared<BroadPhaseCache>();
    BroadPhaseCache& cache = *broadPhaseCache_;
    const auto staticEntityChanged = [&](const auto changedSince) {
        return std::ranges::any_of(changedSince, [&](const Entity entity) {
            return std::ranges::any_of(cache.staticColliders,
                [&](const SceneCollider& collider) { return collider.entity == entity; });
        });
    };
    const bool staticColliderChanged = staticEntityChanged(
        registry.componentEntitiesChangedSince<ColliderComponent>(cache.colliderRevision));
    const bool staticTransformChanged = staticEntityChanged(
        registry.componentEntitiesChangedSince<Transform>(cache.transformRevision));
    const bool cacheInvalid = cache.registry != &registry ||
        cache.structuralRevision != registry.structuralRevision() ||
        staticColliderChanged || staticTransformChanged ||
        cache.rigidbodyRevision != registry.componentRevision<RigidbodyComponent>();
    if (cacheInvalid) {
        cache.registry = &registry;
        cache.structuralRevision = registry.structuralRevision();
        cache.colliderRevision = registry.componentRevision<ColliderComponent>();
        cache.transformRevision = registry.componentRevision<Transform>();
        cache.rigidbodyRevision = registry.componentRevision<RigidbodyComponent>();
        cache.staticColliders.clear();
        registry.view<ColliderComponent, Transform>(
            [&](const Entity entity, const ColliderComponent& collider, const Transform& transform) {
                const bool dynamic = registry.has<RigidbodyComponent>(entity) &&
                    registry.get<RigidbodyComponent>(entity).type == RigidbodyType::Dynamic;
                if (dynamic) return;
                cache.staticColliders.push_back({entity, collider, transform,
                    worldAabb(transform, collider),
                    transform.rotation.y() * 0.01745329251994329577f, false});
            });
    } else {
        // Dynamic bodies are allowed to mark their transforms as changed during
        // contact resolution. Those changes must not rebuild the static cache.
        cache.colliderRevision = registry.componentRevision<ColliderComponent>();
        cache.transformRevision = registry.componentRevision<Transform>();
    }

    colliders = cache.staticColliders;
    registry.view<ColliderComponent, Transform>(
        [&](const Entity entity, const ColliderComponent& collider, const Transform& transform) {
            if (!registry.has<RigidbodyComponent>(entity) ||
                registry.get<RigidbodyComponent>(entity).type != RigidbodyType::Dynamic) return;
            colliders.push_back({entity, collider, transform, worldAabb(transform, collider),
                transform.rotation.y() * 0.01745329251994329577f, true});
        });

    Grid broadPhase;
    broadPhase.cells.reserve(colliders.size() * 2);
    for (std::size_t index = 0; index < colliders.size(); ++index) {
        broadPhase.insert(colliders[index].bounds, index);
    }
    std::vector<std::uint32_t> seen(colliders.size(), 0);
    std::uint32_t queryStamp = 0;
    // Reuse one query buffer for the entire simulation step. Creating this
    // vector inside the body loop used to allocate repeatedly as the number
    // of dynamic bodies grew.
    std::vector<std::size_t> candidates;
    candidates.reserve(colliders.size());

    registry.view<RigidbodyComponent, Transform>(
        [&](const Entity entity, RigidbodyComponent& body, Transform& transform) {
            if (body.type != RigidbodyType::Dynamic) return;

            const Vec3 previousPosition = transform.position;
            const float inverseMass = body.mass > 0.0f ? 1.0f / body.mass : 0.0f;
            Vec3 acceleration = body.force * inverseMass;
            if (body.useGravity) acceleration += gravity_;
            body.linearVelocity += acceleration * dt;
            if (!body.fixedRotation) {
                body.angularVelocity += body.torque * inverseMass * dt;
            }
            transform.position += body.linearVelocity * dt;

            bool touchesSurface = false;
            Vec3 surfaceNormal{0.0f, 1.0f, 0.0f};
            float contactFriction = 0.0f;
            if (registry.has<ColliderComponent>(entity)) {
                const auto& collider = registry.get<ColliderComponent>(entity);
                const bool isSphere = std::holds_alternative<SphereCollider>(collider.shape);
                Aabb bodyBounds = worldAabb(transform, collider);
                Aabb previousBodyBounds = bodyBounds;
                previousBodyBounds.center += previousPosition - transform.position;
                float contactHeight = -INFINITY;
                Vec3 contactNormal{0.0f, 1.0f, 0.0f};
                float selectedFriction = 0.0f;
                float selectedRestitution = 0.0f;
                Vec3 rampCorrection{};
                Vec3 rampNormal{0.0f, 1.0f, 0.0f};
                float rampFriction = 0.0f;
                float rampPenetration = 0.0f;
                candidates.clear();
                ++queryStamp;
                if (queryStamp == 0) {
                    std::fill(seen.begin(), seen.end(), 0);
                    queryStamp = 1;
                }
                broadPhase.query(bodyBounds, candidates, seen, queryStamp);
                for (const std::size_t candidateIndex : candidates) {
                    const SceneCollider& other = colliders[candidateIndex];
                    if (other.entity == entity || !overlapsHorizontally(bodyBounds, other.bounds)) continue;
                    const auto& otherCollider = other.collider;
                    if (collider.isTrigger || otherCollider.isTrigger) continue;
                    // The generic box solver still handles only immovable
                    // surfaces. Sphere contacts below support every collider
                    // in the scene, including colliders on dynamic entities.
                    if (other.dynamic && !isSphere) continue;
                    const bool isRamp = std::holds_alternative<RampCollider>(otherCollider.shape);
                    if (!isSphere && std::holds_alternative<BoxCollider>(collider.shape) &&
                        std::holds_alternative<BoxCollider>(otherCollider.shape)) {
                        if (const auto contact = boxBoxContact(
                                bodyBounds, previousBodyBounds, other.bounds)) {
                            transform.position += contact->normal * contact->penetration;
                            bodyBounds = worldAabb(transform, collider);
                            const OrientedBox box = orientedBox(transform, collider);
                            applyBoxSurfaceImpulse(
                                body, box, contact->normal,
                                boxSupportPoint(box, contact->normal * -1.0f),
                                std::max(collider.restitution, otherCollider.restitution),
                                std::sqrt(std::max(
                                    0.0f, collider.friction * otherCollider.friction)));
                            if (contact->normal.y() > 1e-4f) {
                                touchesSurface = true;
                                surfaceNormal = contact->normal;
                                contactFriction = std::sqrt(std::max(
                                    0.0f, collider.friction * otherCollider.friction));
                            }
                            continue;
                        }
                    }
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
                    if (isRamp && std::holds_alternative<BoxCollider>(collider.shape)) {
                        const OrientedBox box = orientedBox(transform, collider);
                        if (const auto contact =
                                boxRampContact(box, other.bounds, other.yawRadians)) {
                            transform.position += contact->normal * contact->penetration;
                            bodyBounds = worldAabb(transform, collider);
                            const OrientedBox correctedBox = orientedBox(transform, collider);
                            applyBoxSurfaceImpulse(
                                body, correctedBox, contact->normal,
                                contact->point + contact->normal * contact->penetration,
                                std::max(collider.restitution, otherCollider.restitution),
                                std::sqrt(std::max(
                                    0.0f, collider.friction * otherCollider.friction)));
                            if (contact->normal.y() > 1e-4f) {
                                touchesSurface = true;
                                surfaceNormal = contact->normal;
                                contactFriction = std::sqrt(std::max(
                                    0.0f, collider.friction * otherCollider.friction));
                            }
                        }
                        continue;
                    }
                    if (isSphere && !isRamp) {
                        const auto& otherTransform = other.transform;
                        std::optional<SphereContact> contact;
                        std::optional<OrientedBox> contactedBox;
                        if (std::holds_alternative<BoxCollider>(otherCollider.shape)) {
                            contactedBox = orientedBox(otherTransform, otherCollider);
                            contact = sphereOrientedBoxContact(
                                bodyBounds.center, bodyBounds.extents.y(), *contactedBox);
                        } else {
                            contact = sphereColliderContact(
                                bodyBounds.center, bodyBounds.extents.y(), other.bounds,
                                otherCollider);
                        }
                        if (contact) {
                            const bool dynamicBox = other.dynamic && contactedBox.has_value();
                            if (dynamicBox) {
                                auto& otherBody =
                                    registry.get<RigidbodyComponent>(other.entity);
                                auto& mutableOtherTransform =
                                    registry.get<Transform>(other.entity);
                                const float sphereInverseMass =
                                    body.mass > 0.0f ? 1.0f / body.mass : 0.0f;
                                const float boxInverseMass =
                                    otherBody.mass > 0.0f ? 1.0f / otherBody.mass : 0.0f;
                                const float inverseMassSum =
                                    sphereInverseMass + boxInverseMass;
                                if (inverseMassSum > 0.0f) {
                                    transform.position += contact->normal * contact->penetration *
                                        (sphereInverseMass / inverseMassSum);
                                    mutableOtherTransform.position -=
                                        contact->normal * contact->penetration *
                                        (boxInverseMass / inverseMassSum);
                                    registry.markChanged<Transform>(other.entity);
                                }
                            } else {
                                transform.position +=
                                    contact->normal * contact->penetration;
                            }
                            bodyBounds = worldAabb(transform, collider);
                            float restitution = std::max(
                                collider.restitution, otherCollider.restitution);
                            if (!other.dynamic && std::abs(contact->normal.y()) < 0.5f) {
                                restitution = std::max(
                                    restitution, MinimumSphereWallRestitution);
                            }
                            if (dynamicBox) {
                                auto& otherBody =
                                    registry.get<RigidbodyComponent>(other.entity);
                                const Vec3 contactPoint = bodyBounds.center -
                                    contact->normal * bodyBounds.extents.y();
                                applySphereBoxImpulse(body, otherBody, *contactedBox,
                                    contact->normal, contactPoint, restitution);
                            } else {
                                const float inwardVelocity =
                                    dot(body.linearVelocity, contact->normal);
                                if (inwardVelocity < 0.0f) {
                                    body.linearVelocity -= contact->normal *
                                        ((1.0f + restitution) * inwardVelocity);
                                }
                            }
                            if (contact->normal.y() > 1e-4f) {
                                touchesSurface = true;
                                surfaceNormal = contact->normal;
                                contactFriction = std::sqrt(std::max(
                                    0.0f, collider.friction * otherCollider.friction));
                            }
                            continue;
                        }
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
                    const bool isBox = std::holds_alternative<BoxCollider>(collider.shape);
                    const float supportRadius = isBox
                        ? boxSupportRadius(orientedBox(transform, collider), normal)
                        : bodyBounds.extents.y();
                    const float targetCenterY = isSphere || isBox
                        ? otherTop + supportRadius / normal.y()
                        : otherTop + supportRadius;
                    const float previousTop = isRamp
                        ? rampTopAt(other.bounds, previousPosition, other.yawRadians)
                        : other.bounds.center.y() + other.bounds.extents.y();
                    const float previousTargetY = isSphere || isBox
                        ? previousTop + supportRadius / normal.y()
                        : previousTop + supportRadius;
                    const bool approachedFromAbove =
                        previousPosition.y() >= previousTargetY - 1e-4f ||
                        (isBox && previousPosition.y() >= previousTop);
                    if (transform.position.y() <= targetCenterY + 1e-4f &&
                        approachedFromAbove &&
                        dot(body.linearVelocity, normal) <= 1e-4f &&
                        targetCenterY > contactHeight) {
                        // Resolve only the highest eligible surface. Resolving
                        // each ramp immediately could let a later, lower
                        // contact pull the sphere through an adjacent ramp.
                        contactHeight = targetCenterY;
                        contactNormal = normal;
                        selectedFriction = std::sqrt(std::max(0.0f,
                            collider.friction * otherCollider.friction));
                        selectedRestitution = std::max(
                            collider.restitution, otherCollider.restitution);
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
                    if (std::holds_alternative<BoxCollider>(collider.shape)) {
                        const OrientedBox box = orientedBox(transform, collider);
                        applyBoxSurfaceImpulse(
                            body, box, contactNormal,
                            boxSupportPoint(box, contactNormal * -1.0f),
                            selectedRestitution, selectedFriction);
                    } else {
                        const float inwardVelocity = dot(body.linearVelocity, contactNormal);
                        if (inwardVelocity < 0.0f) {
                            body.linearVelocity -= contactNormal * inwardVelocity;
                        }
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

                if (touchesSurface &&
                    std::holds_alternative<BoxCollider>(collider.shape)) {
                    const float normalSpeed = dot(body.linearVelocity, surfaceNormal);
                    const Vec3 tangentialVelocity =
                        body.linearVelocity - surfaceNormal * normalSpeed;
                    const bool nearlyStationary =
                        std::abs(normalSpeed) < RestingBoxNormalSpeed &&
                        tangentialVelocity.length() < RestingBoxTangentialSpeed;
                    if (nearlyStationary) {
                        // Remove the tiny into/out-of-surface oscillation left
                        // by alternating corner contacts. Preserve tangential
                        // motion so boxes can still slide down a ramp.
                        body.linearVelocity = tangentialVelocity;
                        if (!body.fixedRotation &&
                            body.angularVelocity.length() <
                                RestingBoxAngularSpeedDegrees) {
                            body.angularVelocity = {};
                        }
                    }
                }
            }

            if (!body.fixedRotation) {
                transform.rotation += body.angularVelocity * dt;
            }
            // Damping is expressed per second and applied exponentially so
            // that the result remains stable when the frame rate changes.
            body.linearVelocity *= std::exp(-std::max(body.linearDamping, 0.0f) * dt);
            if (!body.fixedRotation) {
                body.angularVelocity *=
                    std::exp(-std::max(body.angularDamping, 0.0f) * dt);
            }
            // addForce/addTorque are frame-step inputs. A script that wants a
            // persistent force should add it again each frame.
            body.zeroForces();
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
