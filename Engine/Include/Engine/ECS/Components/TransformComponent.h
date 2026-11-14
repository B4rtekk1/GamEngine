#pragma once

/** @file TransformComponent.h Position, rotation and scale ECS component. */

#include <Engine/ECS/Entity.h>
#include <Engine/Math/Math.h>
#include <cmath>
#include <cstdint>

namespace Engine {
/**
 * @brief Local transform plus runtime-only cached world-space state.
 *
 * Position, rotation and scale are always relative to ParentComponent.
 * Root entities therefore use them as world values.  The cache is filled
 * by TransformSystem and is deliberately not serialized.
 */
struct TransformComponent {
    /** @brief Local position. */
    Vec3 position;
    /** @brief Local Euler rotation in degrees. */
    Vec3 rotation;
    /** @brief Local per-axis scale. */
    Vec3 scale{1.0F, 1.0F, 1.0F};

    [[nodiscard]] const Mat4& worldMatrix() const noexcept {
        return cachedWorldMatrix;
    }
    /** @brief Returns the translation read directly from the world matrix. */
    [[nodiscard]] const Vec3& worldPosition() const noexcept {
        return cachedWorldPosition;
    }

    /** @brief Lazily decomposes the world matrix to obtain Euler rotation. */
    [[nodiscard]] const Vec3& worldRotation() const noexcept;

    /** @brief Lazily decomposes the world matrix to obtain per-axis scale. */
    [[nodiscard]] const Vec3& worldScale() const noexcept;

    [[nodiscard]] std::uint64_t worldRevision() const noexcept {
        return cachedWorldRevision;
    }

    [[nodiscard]] TransformComponent worldTransform() const noexcept;

    /**
     * @brief Builds the local-space matrix T * Rx * Ry * Rz * S.
     *
     * Closed form of the same product. Three generic axis-angle rotations
     * (each normalizing its axis and multiplying a 4x4 matrix) dominated
     * TransformSystem cost for scenes with many moving bodies.
     */
    [[nodiscard]] Mat4 matrix() const noexcept {
        const glm::vec3 radians = glm::radians(rotation.native());
        const float sx = std::sin(radians.x);
        const float cx = std::cos(radians.x);
        const float sy = std::sin(radians.y);
        const float cy = std::cos(radians.y);
        const float sz = std::sin(radians.z);
        const float cz = std::cos(radians.z);
        const glm::vec3 s = scale.native();
        glm::mat4 result{1.0F};
        result[0] = glm::vec4{cy * cz, sx * sy * cz + cx * sz, sx * sz - cx * sy * cz, 0.0F} * s.x;
        result[1] = glm::vec4{-cy * sz, cx * cz - sx * sy * sz, cx * sy * sz + sx * cz, 0.0F} * s.y;
        result[2] = glm::vec4{sy, -sx * cy, cx * cy, 0.0F} * s.z;
        result[3] = glm::vec4{position.native(), 1.0F};
        return Mat4{result};
    }

    // TransformSystem-owned cache. Keep it public so it remains an ECS
    // value type; clients must consume it through the accessors above.
    Mat4 cachedWorldMatrix;
    Vec3 cachedWorldPosition;
    mutable Vec3 cachedWorldRotation;
    mutable Vec3 cachedWorldScale{1.0F, 1.0F, 1.0F};
    Entity cachedParent{NullEntity};
    std::uint64_t cachedParentWorldRevision{};
    std::uint64_t cachedWorldRevision{};
    bool worldCacheValid{false};
    /** False until worldRotation() or worldScale() requires decomposition. */
    mutable bool worldTrsValid{false};
};
} // namespace Engine
