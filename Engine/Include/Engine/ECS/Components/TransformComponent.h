#pragma once

/** @file TransformComponent.h Position, rotation and scale ECS component. */

#include <Engine/Math/Math.h>
#include <Engine/ECS/Entity.h>

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

        [[nodiscard]] const Mat4 &worldMatrix() const noexcept { return cachedWorldMatrix; }
        [[nodiscard]] const Mat4 &previousWorldMatrix() const noexcept { return previousCachedWorldMatrix; }
        [[nodiscard]] std::uint64_t worldRevision() const noexcept { return cachedWorldRevision; }
        [[nodiscard]] TransformComponent worldTransform() const noexcept {
            return TransformComponent{.position = cachedWorldPosition,
                                      .rotation = cachedWorldRotation,
                                      .scale = cachedWorldScale};
        }

        /** @brief Builds the local-space matrix. */
        [[nodiscard]] Mat4 matrix() const noexcept {
            Mat4 result = Mat4::translate(position);
            result = Mat4::rotate(result, Radians{Degrees{rotation.x()}}, Vec3{1.0F, 0.0F, 0.0F});
            result = Mat4::rotate(result, Radians{Degrees{rotation.y()}}, Vec3{0.0F, 1.0F, 0.0F});
            result = Mat4::rotate(result, Radians{Degrees{rotation.z()}}, Vec3{0.0F, 0.0F, 1.0F});
            return Mat4::scale(result, scale);
        }

        // TransformSystem-owned cache. Keep it public so it remains an ECS
        // value type; clients must consume it through the accessors above.
        Mat4 cachedWorldMatrix{};
        Mat4 previousCachedWorldMatrix{};
        Vec3 cachedWorldPosition{};
        Vec3 cachedWorldRotation{};
        Vec3 cachedWorldScale{1.0F, 1.0F, 1.0F};
        Vec3 cachedLocalPosition{};
        Vec3 cachedLocalRotation{};
        Vec3 cachedLocalScale{1.0F, 1.0F, 1.0F};
        Entity cachedParent{NullEntity};
        std::uint64_t cachedParentWorldRevision{};
        std::uint64_t cachedWorldRevision{};
        bool worldCacheValid{false};
    };
} // namespace Engine
