#pragma once

/** @file AABB.h Axis-aligned bounding box utilities. */

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <limits>

namespace Engine {

/** @brief Axis-aligned bounds in world or local space. */
struct AABB {
    glm::vec3 min{};
    glm::vec3 max{};

    [[nodiscard]] static constexpr AABB unitCube() noexcept {
        return {{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
    }

    /** @brief Returns the enclosing world-space AABB after applying @p transform. */
    [[nodiscard]] AABB transformed(const glm::mat4& transform) const noexcept {
        AABB result{
            .min = glm::vec3{std::numeric_limits<float>::max()},
            .max = glm::vec3{std::numeric_limits<float>::lowest()},
        };

        for (const float x : {min.x, max.x}) {
            for (const float y : {min.y, max.y}) {
                for (const float z : {min.z, max.z}) {
                    const glm::vec3 point = glm::vec3{transform * glm::vec4{x, y, z, 1.0f}};
                    result.min = glm::min(result.min, point);
                    result.max = glm::max(result.max, point);
                }
            }
        }
        return result;
    }
};

} // namespace Engine
