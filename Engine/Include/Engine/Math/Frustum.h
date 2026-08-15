#pragma once

/** @file Frustum.h View-frustum intersection utilities. */


#include <algorithm>
#include <array>

namespace Engine {

/** @brief Six clipping planes extracted from a Vulkan view-projection matrix. */
class Frustum final {
public:
    explicit Frustum(const Mat4& viewProjection) noexcept
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
        return std::ranges::all_of(m_planes, [&bounds](const Vec4& plane) {
            const Vec3 positiveVertex{
                plane.x() >= 0.0f ? bounds.max.x() : bounds.min.x(),
                plane.y() >= 0.0f ? bounds.max.y() : bounds.min.y(),
                plane.z() >= 0.0f ? bounds.max.z() : bounds.min.z(),
            };
            return dot(Vec3{plane.x(), plane.y(), plane.z()}, positiveVertex) + plane.w() >= 0.0f;
        });
    }

private:
    static Vec4 row(const Mat4& matrix, const int index) noexcept {
        const auto& native = matrix.native();
        return {native[0][index], native[1][index], native[2][index], native[3][index]};
    }

    static Vec4 normalizedPlane(const Vec4& plane) noexcept {
        const float length = Vec3{plane.x(), plane.y(), plane.z()}.length();
        return length > 0.0f ? plane / length : plane;
    }

    std::array<Vec4, 6> m_planes;
};

} // namespace Engine
