#pragma once

/** @file Frustum.h View-frustum intersection utilities. */

#include "Engine/Math/AABB.h"

#include <glm/glm.hpp>

#include <array>

namespace Engine {

/** @brief Six clipping planes extracted from a Vulkan view-projection matrix. */
class Frustum final {
public:
    explicit Frustum(const glm::mat4& viewProjection) noexcept
        : m_planes{
            normalizedPlane(row(viewProjection, 3) + row(viewProjection, 0)), // left
            normalizedPlane(row(viewProjection, 3) - row(viewProjection, 0)), // right
            normalizedPlane(row(viewProjection, 3) + row(viewProjection, 1)), // bottom
            normalizedPlane(row(viewProjection, 3) - row(viewProjection, 1)), // top
            normalizedPlane(row(viewProjection, 2)),                           // near (Vulkan: z >= 0)
            normalizedPlane(row(viewProjection, 3) - row(viewProjection, 2)), // far
        } {}

    /** @brief Returns false only when @p bounds lies fully outside a clipping plane. */
    [[nodiscard]] bool intersects(const AABB& bounds) const noexcept {
        for (const glm::vec4& plane : m_planes) {
            const glm::vec3 positiveVertex{
                plane.x >= 0.0f ? bounds.max.x : bounds.min.x,
                plane.y >= 0.0f ? bounds.max.y : bounds.min.y,
                plane.z >= 0.0f ? bounds.max.z : bounds.min.z,
            };
            if (glm::dot(glm::vec3{plane}, positiveVertex) + plane.w < 0.0f) {
                return false;
            }
        }
        return true;
    }

private:
    static glm::vec4 row(const glm::mat4& matrix, const int index) noexcept {
        return {matrix[0][index], matrix[1][index], matrix[2][index], matrix[3][index]};
    }

    static glm::vec4 normalizedPlane(const glm::vec4& plane) noexcept {
        const float length = glm::length(glm::vec3{plane});
        return length > 0.0f ? plane / length : plane;
    }

    std::array<glm::vec4, 6> m_planes;
};

} // namespace Engine
