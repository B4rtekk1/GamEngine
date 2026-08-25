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
constexpr float RadiansToDegrees = 57.29577951308232F;
constexpr float SolidSphereInertiaFactor = 0.4F;
// Rolling resistance is much weaker than sliding Coulomb friction. This
// factor maps the collider friction coefficient to a practical rolling
// resistance coefficient for rigid spheres.
constexpr float RollingResistanceScale = 0.05F;
// A perfectly inelastic side contact makes a rolling sphere appear glued to
// a wall. Keep a subtle rebound on near-vertical surfaces while allowing
// restitution-free floor contacts to remain stable.
constexpr float MinimumSphereWallRestitution = 0.15F;
constexpr float RestingBoxNormalSpeed = 0.2F;
constexpr float RestingBoxTangentialSpeed = 0.1F;
constexpr float RestingBoxAngularSpeedDegrees = 5.0F;

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

constexpr float BroadPhaseCellSize = 4.0F;

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
    const float toRadians = 1.0F / RadiansToDegrees;
    return Quat::angleAxis(transform.rotation.x() * toRadians, {1.0F, 0.0F, 0.0F}) *
           Quat::angleAxis(transform.rotation.y() * toRadians, {0.0F, 1.0F, 0.0F}) *
           Quat::angleAxis(transform.rotation.z() * toRadians, {0.0F, 0.0F, 1.0F});
}

OrientedBox orientedBox(const Transform& transform, const ColliderComponent& collider) {
    const Quat rotation = boxRotation(transform);
    const Vec3 scale{std::abs(transform.scale.x()), std::abs(transform.scale.y()),
                     std::abs(transform.scale.z())};
    return {
        transform.position + rotation * (collider.offset * scale),
        rotation * Vec3{1.0F, 0.0F, 0.0F},
        rotation * Vec3{0.0F, 1.0F, 0.0F},
        rotation * Vec3{0.0F, 0.0F, 1.0F},
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
        if (std::abs(projection) > 1e-5F) {
            result += axis * (projection < 0.0F ? -extent : extent);
        }
    };
    addAxis(box.axisX, box.halfExtents.x());
    addAxis(box.axisY, box.halfExtents.y());
    addAxis(box.axisZ, box.halfExtents.z());
    return result;
}

Vec3 applyInverseBoxInertia(const OrientedBox& box, const float mass,
                            const Vec3& worldVector) {
    if (mass <= 0.0F) return {};
    const float ix = mass / 3.0F *
        (box.halfExtents.y() * box.halfExtents.y() +
         box.halfExtents.z() * box.halfExtents.z());
    const float iy = mass / 3.0F *
        (box.halfExtents.x() * box.halfExtents.x() +
         box.halfExtents.z() * box.halfExtents.z());
    const float iz = mass / 3.0F *
        (box.halfExtents.x() * box.halfExtents.x() +
         box.halfExtents.y() * box.halfExtents.y());
    return box.axisX * (dot(worldVector, box.axisX) / std::max(ix, 1e-6F)) +
           box.axisY * (dot(worldVector, box.axisY) / std::max(iy, 1e-6F)) +
           box.axisZ * (dot(worldVector, box.axisZ) / std::max(iz, 1e-6F));
}

struct Contact {
    // The normal always points from body B towards body A. Keeping one
    // convention lets the response solver work for every collider pair.
    Vec3 normal;
    float penetration = 0.0F;
    Vec3 point;
};

Vec3 colliderCenter(const Transform& transform, const ColliderComponent& collider) {
    const Vec3 scale{std::abs(transform.scale.x()), std::abs(transform.scale.y()),
                     std::abs(transform.scale.z())};
    if (std::holds_alternative<BoxCollider>(collider.shape)) {
        return transform.position + boxRotation(transform) * (collider.offset * scale);
    }
    return transform.position + collider.offset * scale;
}

Vec3 applyInverseInertia(const RigidbodyComponent& body,
                         const Transform& transform,
                         const ColliderComponent& collider,
                         const Vec3& worldVector) {
    if (body.mass <= 0.0F || body.fixedRotation) return {};
    if (std::holds_alternative<BoxCollider>(collider.shape)) {
        return applyInverseBoxInertia(
            orientedBox(transform, collider), body.mass, worldVector);
    }

    const Vec3 scale{std::abs(transform.scale.x()), std::abs(transform.scale.y()),
                     std::abs(transform.scale.z())};
    float inertia = 0.0F;
    if (const auto* sphere = std::get_if<SphereCollider>(&collider.shape)) {
        const float radius = sphere->radius * std::max({scale.x(), scale.y(), scale.z()});
        inertia = SolidSphereInertiaFactor * body.mass * radius * radius;
    } else {
        const Vec3 extents = std::visit([&]<typename T>(const T& shape) {
            using Shape = std::decay_t<T>;
            if constexpr (std::is_same_v<Shape, CapsuleCollider>) {
                return Vec3{shape.radius, shape.height * 0.5F, shape.radius} * scale;
            } else if constexpr (std::is_same_v<Shape, RampCollider>) {
                return shape.halfExtents * scale;
            } else {
                return Vec3{0.5F, 0.5F, 0.5F} * scale;
            }
        }, collider.shape);
        inertia = body.mass / 3.0F *
            (extents.x() * extents.x() + extents.y() * extents.y() +
             extents.z() * extents.z());
    }
    return inertia > 1e-6F ? worldVector * (1.0F / inertia) : Vec3{};
}

float inverseMass(const RigidbodyComponent* body) {
    return body != nullptr && body->type == RigidbodyType::Dynamic && body->mass > 0.0F
        ? 1.0F / body->mass
        : 0.0F;
}

void resolveContact(RigidbodyComponent& bodyA, Transform& transformA,
                    const ColliderComponent& colliderA,
                    RigidbodyComponent* bodyB, Transform* transformB,
                    const ColliderComponent& colliderB,
                    const Contact& contact, const float restitution,
                    const float friction) {
    const float inverseMassA = inverseMass(&bodyA);
    const float inverseMassB = inverseMass(bodyB);
    const float inverseMassSum = inverseMassA + inverseMassB;
    if (inverseMassSum <= 0.0F) return;

    // Positional correction is mass-weighted, so the same code supports a
    // dynamic body against a static surface and two dynamic bodies.
    transformA.position += contact.normal *
        (contact.penetration * inverseMassA / inverseMassSum);
    if (transformB != nullptr && inverseMassB > 0.0F) {
        transformB->position -= contact.normal *
            (contact.penetration * inverseMassB / inverseMassSum);
    }

    const Vec3 centerA = colliderCenter(transformA, colliderA);
    const Vec3 centerB = transformB != nullptr
        ? colliderCenter(*transformB, colliderB)
        : contact.point;
    const Vec3 leverA = contact.point - centerA;
    const Vec3 leverB = contact.point - centerB;
    Vec3 angularA = bodyA.angularVelocity * (1.0F / RadiansToDegrees);
    Vec3 angularB = bodyB != nullptr
        ? bodyB->angularVelocity * (1.0F / RadiansToDegrees)
        : Vec3{};

    const auto relativeVelocity = [&] {
        const Vec3 velocityA = bodyA.linearVelocity + cross(angularA, leverA);
        const Vec3 velocityB = bodyB != nullptr
            ? bodyB->linearVelocity + cross(angularB, leverB)
            : Vec3{};
        return velocityA - velocityB;
    };
    const auto angularDenominator = [&](const Vec3& direction) {
        float result = 0.0F;
        if (!bodyA.fixedRotation) {
            result += dot(direction, cross(
                applyInverseInertia(bodyA, transformA, colliderA,
                    cross(leverA, direction)), leverA));
        }
        if (bodyB != nullptr && transformB != nullptr && !bodyB->fixedRotation) {
            result += dot(direction, cross(
                applyInverseInertia(*bodyB, *transformB, colliderB,
                    cross(leverB, direction)), leverB));
        }
        return result;
    };
    const auto applyImpulse = [&](const Vec3& impulse) {
        bodyA.linearVelocity += impulse * inverseMassA;
        if (!bodyA.fixedRotation) {
            angularA += applyInverseInertia(
                bodyA, transformA, colliderA, cross(leverA, impulse));
        }
        if (bodyB != nullptr && transformB != nullptr && inverseMassB > 0.0F) {
            bodyB->linearVelocity -= impulse * inverseMassB;
            if (!bodyB->fixedRotation) {
                angularB -= applyInverseInertia(
                    *bodyB, *transformB, colliderB, cross(leverB, impulse));
            }
        }
    };

    const float normalSpeed = dot(relativeVelocity(), contact.normal);
    float normalImpulseMagnitude = 0.0F;
    if (normalSpeed < 0.0F) {
        normalImpulseMagnitude = -(1.0F + restitution) * normalSpeed /
            std::max(inverseMassSum + angularDenominator(contact.normal), 1e-6F);
        applyImpulse(contact.normal * normalImpulseMagnitude);
    }

    // Spheres use the rolling constraint after contact resolution. Applying
    // Coulomb friction here as well would count the same contact twice.
    const float impulseFriction =
        std::holds_alternative<SphereCollider>(colliderA.shape) ? 0.0F : friction;
    Vec3 tangent = relativeVelocity() -
        contact.normal * dot(relativeVelocity(), contact.normal);
    const float tangentSpeed = tangent.length();
    if (tangentSpeed > 1e-6F && impulseFriction > 0.0F &&
        normalImpulseMagnitude > 0.0F) {
        tangent *= 1.0F / tangentSpeed;
        const float unconstrainedImpulse = -dot(relativeVelocity(), tangent) /
            std::max(inverseMassSum + angularDenominator(tangent), 1e-6F);
        const float frictionImpulse = std::clamp(
            unconstrainedImpulse,
            -impulseFriction * normalImpulseMagnitude,
            impulseFriction * normalImpulseMagnitude);
        applyImpulse(tangent * frictionImpulse);
    }

    if (!bodyA.fixedRotation) {
        bodyA.angularVelocity = angularA * RadiansToDegrees;
    }
    if (bodyB != nullptr && !bodyB->fixedRotation) {
        bodyB->angularVelocity = angularB * RadiansToDegrees;
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
            return Vec3{shape.radius, shape.height * 0.5F, shape.radius};
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
    return Vec3{std::sin(yawRadians) * localZ, 1.0F,
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

std::array<Vec3, 8> boxVertices(const OrientedBox& box) {
    std::array<Vec3, 8> vertices{};
    std::size_t index = 0;
    for (const float x : {-1.0F, 1.0F}) {
        for (const float y : {-1.0F, 1.0F}) {
            for (const float z : {-1.0F, 1.0F}) {
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

std::optional<Contact> boxRampContact(
    const OrientedBox& box, const Aabb& ramp, const float yawRadians) {
    if (ramp.extents.x() <= 0.0F || ramp.extents.y() <= 0.0F ||
        ramp.extents.z() <= 0.0F) {
        return std::nullopt;
    }

    const auto boxPoints = boxVertices(box);
    const auto rampPoints = rampVertices(ramp, yawRadians);
    const float slope = ramp.extents.y() / ramp.extents.z();
    const std::array<Vec3, 3> boxAxes{box.axisX, box.axisY, box.axisZ};
    const std::array<Vec3, 4> rampFaceAxes{
        rampWorldDirection({1.0F, 0.0F, 0.0F}, yawRadians),
        rampWorldDirection({0.0F, 1.0F, 0.0F}, yawRadians),
        rampWorldDirection({0.0F, 0.0F, 1.0F}, yawRadians),
        rampWorldDirection({0.0F, 1.0F, -slope}, yawRadians).normalized()
    };
    const std::array<Vec3, 4> rampEdgeAxes{
        rampWorldDirection({1.0F, 0.0F, 0.0F}, yawRadians),
        rampWorldDirection({0.0F, 1.0F, 0.0F}, yawRadians),
        rampWorldDirection({0.0F, 0.0F, 1.0F}, yawRadians),
        rampWorldDirection({0.0F, slope, 1.0F}, yawRadians).normalized()
    };

    float smallestOverlap = INFINITY;
    Vec3 smallestAxis{};
    const auto testAxis = [&](Vec3 axis) {
        const float length = axis.length();
        if (length <= 1e-5F) return true;
        axis *= 1.0F / length;
        const auto [boxMinimum, boxMaximum] = projectedRange(boxPoints, axis);
        const auto [rampMinimum, rampMaximum] = projectedRange(rampPoints, axis);
        const float overlap = std::min(boxMaximum, rampMaximum) -
            std::max(boxMinimum, rampMinimum);
        if (overlap <= 0.0F) return false;
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

    if (dot(smallestAxis, box.center - ramp.center) < 0.0F) {
        smallestAxis *= -1.0F;
    }
    return Contact{
        smallestAxis,
        smallestOverlap,
        boxSupportPoint(box, smallestAxis * -1.0F)
    };
}

std::optional<Contact> boxBoxContact(
    const Aabb& box, const Aabb& previousBox, const Aabb& other) {
    const Vec3 separation = box.center - other.center;
    const Vec3 overlap{
        box.extents.x() + other.extents.x() - std::abs(separation.x()),
        box.extents.y() + other.extents.y() - std::abs(separation.y()),
        box.extents.z() + other.extents.z() - std::abs(separation.z())};
    if (overlap.x() <= 0.0F || overlap.y() <= 0.0F || overlap.z() <= 0.0F) {
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

    const float sign = current[selectedAxis] < 0.0F ? -1.0F : 1.0F;
    Vec3 normal{};
    if (selectedAxis == 0) normal = {sign, 0.0F, 0.0F};
    else if (selectedAxis == 1) normal = {0.0F, sign, 0.0F};
    else normal = {0.0F, 0.0F, sign};
    return Contact{normal, selectedPenetration};
}

std::optional<Contact> sphereBoxContact(
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
    if (distance > 1e-6F) {
        if (distance >= sphereRadius) return std::nullopt;
        return Contact{separation * (1.0F / distance), sphereRadius - distance};
    }

    // The sphere center is inside the box. Push it through the nearest face;
    // using only the closest-point test would leave it trapped there.
    const Vec3 local = sphereCenter - box.center;
    const float xDistance = box.extents.x() - std::abs(local.x());
    const float yDistance = box.extents.y() - std::abs(local.y());
    const float zDistance = box.extents.z() - std::abs(local.z());
    if (xDistance <= yDistance && xDistance <= zDistance) {
        return Contact{{local.x() < 0.0F ? -1.0F : 1.0F, 0.0F, 0.0F},
                       sphereRadius + xDistance};
    }
    if (yDistance <= zDistance) {
        return Contact{{0.0F, local.y() < 0.0F ? -1.0F : 1.0F, 0.0F},
                       sphereRadius + yDistance};
    }
    return Contact{{0.0F, 0.0F, local.z() < 0.0F ? -1.0F : 1.0F},
                   sphereRadius + zDistance};
}

std::optional<Contact> sphereOrientedBoxContact(
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
    if (distance > 1e-6F) {
        if (distance >= sphereRadius) return std::nullopt;
        return Contact{separation * (1.0F / distance), sphereRadius - distance};
    }

    const float xDistance = box.halfExtents.x() - std::abs(local.x());
    const float yDistance = box.halfExtents.y() - std::abs(local.y());
    const float zDistance = box.halfExtents.z() - std::abs(local.z());
    if (xDistance <= yDistance && xDistance <= zDistance) {
        return Contact{box.axisX * (local.x() < 0.0F ? -1.0F : 1.0F),
                       sphereRadius + xDistance};
    }
    if (yDistance <= zDistance) {
        return Contact{box.axisY * (local.y() < 0.0F ? -1.0F : 1.0F),
                       sphereRadius + yDistance};
    }
    return Contact{box.axisZ * (local.z() < 0.0F ? -1.0F : 1.0F),
                   sphereRadius + zDistance};
}

std::optional<Contact> sphereSphereContact(
    const Vec3& center, const float radius, const Aabb& other) {
    const float otherRadius = std::max({other.extents.x(), other.extents.y(), other.extents.z()});
    const Vec3 separation = center - other.center;
    const float distance = separation.length();
    const float combinedRadius = radius + otherRadius;
    if (distance >= combinedRadius) return std::nullopt;
    const Vec3 normal = distance > 1e-6F
        ? separation * (1.0F / distance)
        : Vec3{0.0F, 1.0F, 0.0F};
    return Contact{normal, combinedRadius - distance};
}

std::optional<Contact> sphereCapsuleContact(
    const Vec3& center, const float radius, const Aabb& capsule) {
    const float capsuleRadius = std::max(capsule.extents.x(), capsule.extents.z());
    const float segmentHalfHeight = std::max(0.0F, capsule.extents.y() - capsuleRadius);
    const Vec3 closest{capsule.center.x(),
        std::clamp(center.y(), capsule.center.y() - segmentHalfHeight,
                   capsule.center.y() + segmentHalfHeight),
        capsule.center.z()};
    const Vec3 separation = center - closest;
    const float distance = separation.length();
    const float combinedRadius = radius + capsuleRadius;
    if (distance >= combinedRadius) return std::nullopt;
    const Vec3 normal = distance > 1e-6F
        ? separation * (1.0F / distance)
        : Vec3{0.0F, 1.0F, 0.0F};
    return Contact{normal, combinedRadius - distance};
}

std::optional<Contact> sphereColliderContact(
    const Vec3& center, const float radius, const Aabb& other,
    const ColliderComponent& otherCollider) {
    return std::visit([&]<typename T>([[maybe_unused]] const T& shape) -> std::optional<Contact> {
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

std::optional<Contact> sphereRampContact(
    const Aabb& ramp, const Vec3& sphereCenter, const float radius,
    const Vec3& previousCenter, const float yawRadians) {
    if (radius <= 0.0F || ramp.extents.z() <= 0.0F) return std::nullopt;

    const Vec3 local = rampLocalPosition(ramp, sphereCenter, yawRadians);
    const Vec3 previousLocal = rampLocalPosition(ramp, previousCenter, yawRadians);
    const float slope = ramp.extents.y() / ramp.extents.z();

    // Ignore contacts entered through the solid underside. A small amount of
    // penetration is allowed so that discrete time steps do not lose a valid
    // contact with the sloped face.
    const float planeScale = std::sqrt(1.0F + slope * slope);
    const float previousPlaneDistance =
        (previousLocal.y() - slope * previousLocal.z()) / planeScale;
    if (previousPlaneDistance < -radius) return std::nullopt;

    const float closestX = std::clamp(local.x(), -ramp.extents.x(), ramp.extents.x());
    const float closestZ = std::clamp(
        (local.z() + slope * local.y()) / (1.0F + slope * slope),
        -ramp.extents.z(), ramp.extents.z());
    const Vec3 closestPoint{closestX, slope * closestZ, closestZ};
    const Vec3 separation = local - closestPoint;
    const float distance = separation.length();
    if (distance >= radius) return std::nullopt;

    const Vec3 localNormal = distance > 1e-6F
        ? separation * (1.0F / distance)
        : Vec3{0.0F, 1.0F, -slope}.normalized();
    if (localNormal.y() <= 0.0F) return std::nullopt;

    return Contact{
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
    const float dt = std::max(deltaTime, 0.0F);
    if (dt == 0.0F) return;

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
                    transform.rotation.y() * 0.01745329251994329577F, false});
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
                transform.rotation.y() * 0.01745329251994329577F, true});
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
            const float inverseMass = body.mass > 0.0F ? 1.0F / body.mass : 0.0F;
            Vec3 acceleration = body.force * inverseMass;
            if (body.useGravity) acceleration += gravity_;
            body.linearVelocity += acceleration * dt;
            if (!body.fixedRotation) {
                body.angularVelocity += body.torque * inverseMass * dt;
            }
            transform.position += body.linearVelocity * dt;

            bool touchesSurface = false;
            Vec3 surfaceNormal{0.0F, 1.0F, 0.0F};
            float contactFriction = 0.0F;
            if (registry.has<ColliderComponent>(entity)) {
                const auto& collider = registry.get<ColliderComponent>(entity);
                const bool isSphere = std::holds_alternative<SphereCollider>(collider.shape);
                Aabb bodyBounds = worldAabb(transform, collider);
                Aabb previousBodyBounds = bodyBounds;
                previousBodyBounds.center += previousPosition - transform.position;
                float contactHeight = -INFINITY;
                Vec3 contactNormal{0.0F, 1.0F, 0.0F};
                float selectedFriction = 0.0F;
                float selectedRestitution = 0.0F;
                const SceneCollider* selectedSurface = nullptr;
                Vec3 rampNormal{0.0F, 1.0F, 0.0F};
                float rampFriction = 0.0F;
                float rampPenetration = 0.0F;
                const SceneCollider* selectedRamp = nullptr;
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
                    // Assign every dynamic pair to the shape path that can
                    // generate its contact. A sphere owns sphere-vs-box and
                    // sphere-vs-capsule contacts regardless of entity order;
                    // equal-shape pairs use the entity id only for deduping.
                    // This is important because bodies are integrated in ECS
                    // order: skipping a later sphere could miss the contact
                    // created by its movement during this step.
                    if (other.dynamic) {
                        const bool otherIsSphere =
                            std::holds_alternative<SphereCollider>(otherCollider.shape);
                        if ((isSphere && otherIsSphere && entity > other.entity) ||
                            (!isSphere && otherIsSphere) ||
                            (!isSphere && !otherIsSphere && entity > other.entity)) {
                            continue;
                        }
                    }
                    const bool isRamp = std::holds_alternative<RampCollider>(otherCollider.shape);
                    if (!isSphere && std::holds_alternative<BoxCollider>(collider.shape) &&
                        std::holds_alternative<BoxCollider>(otherCollider.shape)) {
                        if (const auto contact = boxBoxContact(
                                bodyBounds, previousBodyBounds, other.bounds)) {
                            const OrientedBox box = orientedBox(transform, collider);
                            RigidbodyComponent* otherBody = other.dynamic
                                ? &registry.get<RigidbodyComponent>(other.entity)
                                : nullptr;
                            Transform* otherTransform = other.dynamic
                                ? &registry.get<Transform>(other.entity)
                                : nullptr;
                            resolveContact(body, transform, collider,
                                otherBody, otherTransform, otherCollider,
                                Contact{contact->normal, contact->penetration,
                                    boxSupportPoint(box, contact->normal * -1.0F)},
                                std::max(collider.restitution, otherCollider.restitution),
                                std::sqrt(std::max(
                                    0.0F, collider.friction * otherCollider.friction)));
                            if (other.dynamic) {
                                registry.markChanged<Transform>(other.entity);
                            }
                            bodyBounds = worldAabb(transform, collider);
                            if (contact->normal.y() > 1e-4F) {
                                touchesSurface = true;
                                surfaceNormal = contact->normal;
                                contactFriction = std::sqrt(std::max(
                                    0.0F, collider.friction * otherCollider.friction));
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
                            rampNormal = contact->normal;
                            rampFriction = std::sqrt(std::max(0.0F,
                                collider.friction * otherCollider.friction));
                            selectedRamp = &other;
                        }
                        continue;
                    }
                    if (isRamp && std::holds_alternative<BoxCollider>(collider.shape)) {
                        const OrientedBox box = orientedBox(transform, collider);
                        if (const auto contact =
                                boxRampContact(box, other.bounds, other.yawRadians)) {
                            resolveContact(body, transform, collider,
                                nullptr, nullptr, otherCollider,
                                Contact{contact->normal, contact->penetration, contact->point},
                                std::max(collider.restitution, otherCollider.restitution),
                                std::sqrt(std::max(
                                    0.0F, collider.friction * otherCollider.friction)));
                            bodyBounds = worldAabb(transform, collider);
                            if (contact->normal.y() > 1e-4F) {
                                touchesSurface = true;
                                surfaceNormal = contact->normal;
                                contactFriction = std::sqrt(std::max(
                                    0.0F, collider.friction * otherCollider.friction));
                            }
                        }
                        continue;
                    }
                    if (isSphere && !isRamp) {
                        const auto& otherTransform = other.transform;
                        std::optional<Contact> contact;
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
                            RigidbodyComponent* otherBody = other.dynamic
                                ? &registry.get<RigidbodyComponent>(other.entity)
                                : nullptr;
                            Transform* otherTransform = other.dynamic
                                ? &registry.get<Transform>(other.entity)
                                : nullptr;
                            float restitution = std::max(
                                collider.restitution, otherCollider.restitution);
                            if (!other.dynamic && std::abs(contact->normal.y()) < 0.5F) {
                                restitution = std::max(
                                    restitution, MinimumSphereWallRestitution);
                            }
                            resolveContact(body, transform, collider,
                                otherBody, otherTransform, otherCollider,
                                Contact{contact->normal, contact->penetration,
                                    bodyBounds.center -
                                        contact->normal * bodyBounds.extents.y()},
                                restitution,
                                std::sqrt(std::max(
                                    0.0F, collider.friction * otherCollider.friction)));
                            if (other.dynamic) {
                                registry.markChanged<Transform>(other.entity);
                            }
                            bodyBounds = worldAabb(transform, collider);
                            if (contact->normal.y() > 1e-4F) {
                                touchesSurface = true;
                                surfaceNormal = contact->normal;
                                contactFriction = std::sqrt(std::max(
                                    0.0F, collider.friction * otherCollider.friction));
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
                                               : Vec3{0.0F, 1.0F, 0.0F};
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
                        previousPosition.y() >= previousTargetY - 1e-4F ||
                        (isBox && previousPosition.y() >= previousTop);
                    if (transform.position.y() <= targetCenterY + 1e-4F &&
                        approachedFromAbove &&
                        dot(body.linearVelocity, normal) <= 1e-4F &&
                        targetCenterY > contactHeight) {
                        // Resolve only the highest eligible surface. Resolving
                        // each ramp immediately could let a later, lower
                        // contact pull the sphere through an adjacent ramp.
                        contactHeight = targetCenterY;
                        contactNormal = normal;
                        selectedFriction = std::sqrt(std::max(0.0F,
                            collider.friction * otherCollider.friction));
                        selectedRestitution = std::max(
                            collider.restitution, otherCollider.restitution);
                        selectedSurface = &other;
                    }
                }

                if (rampPenetration > 0.0F && selectedRamp != nullptr) {
                    const Vec3 contactPoint = bodyBounds.center -
                        rampNormal * bodyBounds.extents.y();
                    resolveContact(body, transform, collider,
                        nullptr, nullptr, selectedRamp->collider,
                        Contact{rampNormal, rampPenetration, contactPoint},
                        std::max(collider.restitution,
                            selectedRamp->collider.restitution),
                        rampFriction);
                    bodyBounds = worldAabb(transform, collider);
                    touchesSurface = true;
                    surfaceNormal = rampNormal;
                    contactFriction = rampFriction;
                }

                // A sphere can touch the ramp and the ground at the same
                // time near the ramp's lower edge. Resolve both contacts;
                // otherwise the ramp contact can mask the ground for one
                // frame and let the sphere tunnel below it.
                if (contactHeight > -INFINITY && selectedSurface != nullptr) {
                    const float penetration = std::max(
                        0.0F, contactHeight - transform.position.y()) /
                        std::max(contactNormal.y(), 1e-6F);
                    const Vec3 contactPoint =
                        std::holds_alternative<BoxCollider>(collider.shape)
                            ? boxSupportPoint(orientedBox(transform, collider),
                                contactNormal * -1.0F)
                            : bodyBounds.center -
                                contactNormal * bodyBounds.extents.y();
                    resolveContact(body, transform, collider,
                        nullptr, nullptr, selectedSurface->collider,
                        Contact{contactNormal, penetration, contactPoint},
                        selectedRestitution, selectedFriction);
                    bodyBounds = worldAabb(transform, collider);
                    touchesSurface = true;
                    surfaceNormal = contactNormal;
                    contactFriction = selectedFriction;
                }

                if (touchesSurface && std::holds_alternative<SphereCollider>(collider.shape) &&
                    !body.fixedRotation) {
                    const float radius = bodyBounds.extents.y();
                    if (radius > 0.0F) {
                        const Vec3 gravityParallel = body.useGravity
                            ? gravity_ - surfaceNormal * dot(gravity_, surfaceNormal)
                            : Vec3{};
                        // A solid sphere rolls without slipping if static
                        // friction can provide the required tangential force.
                        // I = 2/5 mr^2, hence a = g_parallel / (1 + I/mr^2)
                        // = 5/7 g_parallel.
                        const float requiredFriction =
                            SolidSphereInertiaFactor / (1.0F + SolidSphereInertiaFactor) *
                            gravityParallel.length();
                        const float availableFriction = contactFriction *
                            std::abs(dot(gravity_, surfaceNormal));
                        const bool rollsWithoutSlipping = contactFriction > 1e-5F &&
                            availableFriction + 1e-5F >= requiredFriction;
                        if (rollsWithoutSlipping && body.useGravity) {
                            body.linearVelocity -= gravityParallel *
                                (SolidSphereInertiaFactor / (1.0F + SolidSphereInertiaFactor) * dt);

                            Vec3 tangentialVelocity = body.linearVelocity -
                                surfaceNormal * dot(body.linearVelocity, surfaceNormal);
                            const float tangentialSpeed = tangentialVelocity.length();
                            if (tangentialSpeed > 1e-6F) {
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
            body.linearVelocity *= std::exp(-std::max(body.linearDamping, 0.0F) * dt);
            if (!body.fixedRotation) {
                body.angularVelocity *=
                    std::exp(-std::max(body.angularDamping, 0.0F) * dt);
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
    if (directionLength <= 0.0F || maxDistance <= 0.0F) return std::nullopt;
    direction *= 1.0F / directionLength;

    std::optional<RaycastHit> result;
    float nearest = maxDistance;
    scene.registry().view<ColliderComponent, Transform>(
        [&](const Entity entity, const ColliderComponent& collider, const Transform& transform) {
            const Aabb bounds = worldAabb(transform, collider);
            float nearHit = 0.0F;
            float farHit = nearest;
            int hitAxis = -1;
            float hitSign = 0.0F;
            const float origins[3]{origin.x(), origin.y(), origin.z()};
            const float directions[3]{direction.x(), direction.y(), direction.z()};
            const float centers[3]{bounds.center.x(), bounds.center.y(), bounds.center.z()};
            const float extents[3]{bounds.extents.x(), bounds.extents.y(), bounds.extents.z()};

            for (int axis = 0; axis < 3; ++axis) {
                if (std::abs(directions[axis]) < 1e-6F) {
                    if (origins[axis] < centers[axis] - extents[axis] ||
                        origins[axis] > centers[axis] + extents[axis]) return;
                    continue;
                }
                const float inv = 1.0F / directions[axis];
                float t0 = (centers[axis] - extents[axis] - origins[axis]) * inv;
                float t1 = (centers[axis] + extents[axis] - origins[axis]) * inv;
                const float sign = directions[axis] > 0.0F ? -1.0F : 1.0F;
                if (t0 > t1) std::swap(t0, t1);
                if (t0 > nearHit) { nearHit = t0; hitAxis = axis; hitSign = sign; }
                farHit = std::min(farHit, t1);
                if (nearHit > farHit) return;
            }
            if (farHit < 0.0F || nearHit >= nearest) return;
            if (nearHit < 0.0F) nearHit = 0.0F;
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