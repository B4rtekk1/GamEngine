#pragma once

#include "Engine/Renderer/Geometry/Mesh.h"

#include <algorithm>
#include <cmath>

namespace Engine {
namespace detail {
/** Shared lathe builder: a Y-aligned solid with a flat bottom cap and an optional flat top cap. */
[[nodiscard]] inline Mesh createLatheSolid(const float topRadius, const float bottomRadius, const float height,
                                           const unsigned int segments, const bool topCap) {
    constexpr float Pi = 3.14159265358979323846F;
    const unsigned int sides = std::max(segments, 3U);
    const float halfHeight = height * 0.5F;
    // Slope of the side wall: used to tilt normals for cones.
    const float slope = (bottomRadius - topRadius) / std::max(height, 0.0001F);
    const float normalLength = std::sqrt(1.0F + slope * slope);

    Mesh mesh;
    const auto vertex = [&](const float x, const float y, const float z, const float nx, const float ny,
                            const float nz, const float u, const float v) {
        mesh.vertices.push_back({
            .position = {x, y, z},
            .color = {0.80F, 0.80F, 0.80F},
            .texCoord = {u, v},
            .normal = {nx, ny, nz},
        });
    };

    // Side wall: top row then bottom row, same index pattern as Capsule.
    for (unsigned int row = 0; row < 2; ++row) {
        const float radius = row == 0 ? topRadius : bottomRadius;
        const float y = row == 0 ? halfHeight : -halfHeight;
        for (unsigned int segment = 0; segment <= sides; ++segment) {
            const float u = static_cast<float>(segment) / static_cast<float>(sides);
            const float cosA = std::cos(u * Pi * 2.0F);
            const float sinA = std::sin(u * Pi * 2.0F);
            vertex(radius * cosA, y, radius * sinA, cosA / normalLength, (slope) / normalLength, sinA / normalLength, u,
                   static_cast<float>(row));
        }
    }
    for (unsigned int segment = 0; segment < sides; ++segment) {
        const unsigned int first = segment;
        const unsigned int second = first + sides + 1U;
        mesh.indices.insert(mesh.indices.end(), {first, first + 1U, second, first + 1U, second + 1U, second});
    }

    const auto addCap = [&](const bool top) {
        const float radius = top ? topRadius : bottomRadius;
        const float y = top ? halfHeight : -halfHeight;
        const float ny = top ? 1.0F : -1.0F;
        const auto centerIndex = static_cast<unsigned int>(mesh.vertices.size());
        vertex(0.0F, y, 0.0F, 0.0F, ny, 0.0F, 0.5F, 0.5F);
        for (unsigned int segment = 0; segment <= sides; ++segment) {
            const float angle = static_cast<float>(segment) / static_cast<float>(sides) * Pi * 2.0F;
            vertex(radius * std::cos(angle), y, radius * std::sin(angle), 0.0F, ny, 0.0F,
                   0.5F + 0.5F * std::cos(angle), 0.5F + 0.5F * std::sin(angle));
        }
        for (unsigned int segment = 0; segment < sides; ++segment) {
            const unsigned int a = centerIndex + 1U + segment;
            const unsigned int b = a + 1U;
            if (top)
                mesh.indices.insert(mesh.indices.end(), {centerIndex, b, a});
            else
                mesh.indices.insert(mesh.indices.end(), {centerIndex, a, b});
        }
    };
    if (topCap)
        addCap(true);
    addCap(false);
    mesh.recalculateLocalBounds();
    return mesh;
}
} // namespace detail

/** A Y-aligned cylinder centred at the origin. */
class Cylinder final {
  public:
    [[nodiscard]] static Mesh createMesh(const float radius = 0.5F, const float height = 1.0F,
                                         const unsigned int segments = 48U) {
        return detail::createLatheSolid(radius, radius, height, segments, true);
    }
};

/** A Y-aligned cone centred at the origin, apex up. */
class Cone final {
  public:
    [[nodiscard]] static Mesh createMesh(const float radius = 0.5F, const float height = 1.0F,
                                         const unsigned int segments = 48U) {
        return detail::createLatheSolid(0.0F, radius, height, segments, false);
    }
};
} // namespace Engine
