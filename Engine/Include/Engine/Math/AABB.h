#pragma once

/** @file AABB.h Axis-aligned bounding box utilities. */

#include <algorithm>
#include <limits>

#include "Engine/Math/Vec3.h"

namespace Engine {

/** @brief Axis-aligned bounds in world or local space. */
struct AABB {
    Vec3 min{};
    Vec3 max{};

    [[nodiscard]] static constexpr AABB unitCube() noexcept {
        return {{-0.5F, -0.5F, -0.5F}, {0.5F, 0.5F, 0.5F}};
    }

    /** @brief Returns the enclosing world-space AABB after applying @p transform. */
    [[nodiscard]] AABB transformed(const glm::mat4& transform) const noexcept {
        AABB result{
            .min = Vec3{
                std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max(),
            },
            .max = Vec3{
                std::numeric_limits<float>::lowest(),
                std::numeric_limits<float>::lowest(),
                std::numeric_limits<float>::lowest(),
            },
        };

        for (const float x : {min.x(), max.x()}) {
            for (const float y : {min.y(), max.y()}) {
                for (const float z : {min.z(), max.z()}) {
                    const Vec3 point{transform * glm::vec4{x, y, z, 1.0F}};
                    result.min.setX(std::min(result.min.x(), point.x()));
                    result.min.setY(std::min(result.min.y(), point.y()));
                    result.min.setZ(std::min(result.min.z(), point.z()));
                    result.max.setX(std::max(result.max.x(), point.x()));
                    result.max.setY(std::max(result.max.y(), point.y()));
                    result.max.setZ(std::max(result.max.z(), point.z()));
                }
            }
        }
        return result;
    }
};

} // namespace Engine