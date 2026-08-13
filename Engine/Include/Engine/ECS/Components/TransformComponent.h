#pragma once

/** @file TransformComponent.h Position, rotation and scale ECS component. */

#include "Engine/Math/Vec3.h"
#include "Engine/Math/Radians.h"
#include <Engine/Math/Mat4.h>

namespace Engine {

/** @brief World-space position, Euler rotation (degrees), and per-axis scale. */
struct TransformComponent {
    /** @brief World-space position. */
    Vec3 position{};
    /** @brief Euler rotation in degrees. */
    Vec3 rotation{};
    /** @brief Per-axis scale. */
    Vec3 scale{1.0f, 1.0f, 1.0f};

    /** @brief Builds the local-to-world transform matrix. */
    [[nodiscard]] Mat4 matrix() const noexcept {
        Mat4 result = Mat4::translate(position);
        result = Mat4::rotate(result, Radians::fromDegrees(rotation.x()), Vec3{1.0f, 0.0f, 0.0f});
        result = Mat4::rotate(result, Radians::fromDegrees(rotation.y()), Vec3{0.0f, 1.0f, 0.0f});
        result = Mat4::rotate(result, Radians::fromDegrees(rotation.z()), Vec3{0.0f, 0.0f, 1.0f});
        return Mat4::scale(result, scale);
    }
};

} // namespace Engine
