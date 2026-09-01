#pragma once

#include "Engine/Renderer/Geometry/Mesh.h"

#include <algorithm>
#include <cmath>

namespace Engine {
    /** A Y-aligned capsule with hemispherical ends. */
    class Capsule final {
    public:
        /**
         * Creates a capsule centred at the origin.
         *
         * @param radius Radius of the cylinder and the two hemispheres.
         * @param height Total height, including the hemispherical ends.
         * @param hemisphereRings Tessellation per hemispherical end.
         * @param segments Radial tessellation.
         */
        [[nodiscard]] static Mesh createMesh(const float radius = 0.5F,
                                             const float height = 2.0F,
                                             const unsigned int hemisphereRings = 16U,
                                             const unsigned int segments = 48U) {
            constexpr float CapsulePi = 3.14159265358979323846F;
            const unsigned int rings = std::max(hemisphereRings, 1U);
            const unsigned int sides = std::max(segments, 3U);
            const float safeRadius = std::max(radius, 0.001F);
            const float cylinderHalfHeight = std::max((height * 0.5F) - safeRadius, 0.0F);

            Mesh mesh;
            using Size = decltype(mesh.vertices)::size_type;
            const Size rows = static_cast<Size>((rings + 1U) * 2U);
            mesh.vertices.reserve(rows * static_cast<Size>(sides + 1U));
            mesh.indices.reserve((rows - 1U) * static_cast<Size>(sides) * 6U);

            const auto addHemisphere = [&](const bool top) {
                for (unsigned int ring = 0; ring <= rings; ++ring) {
                    const float angle = (static_cast<float>(ring) / static_cast<float>(rings)) * (CapsulePi * 0.5F);
                    const float radial = std::sin(angle);
                    const float vertical = std::cos(angle);
                    for (unsigned int segment = 0; segment <= sides; ++segment) {
                        const float u = static_cast<float>(segment) / static_cast<float>(sides);
                        const float azimuth = u * CapsulePi * 2.0F;
                        const float x = radial * std::cos(azimuth);
                        const float z = radial * std::sin(azimuth);
                        const float y = top ? vertical : -vertical;
                        mesh.vertices.push_back({
                            .position = {x * safeRadius, (y * safeRadius) +
                                                       (top ? cylinderHalfHeight : -cylinderHalfHeight),
                                         z * safeRadius},
                            .color = {0.45F, 0.80F, 0.48F},
                            .texCoord = {u, top ? 0.5F * (1.0F - (static_cast<float>(ring) / rings))
                                                        : 0.5F + (0.5F * (static_cast<float>(ring) / rings))},
                            .normal = {x, y, z},
                        });
                    }
                }
            };
            addHemisphere(true);
            addHemisphere(false);

            for (unsigned int row = 0; row + 1U < rows; ++row) {
                for (unsigned int segment = 0; segment < sides; ++segment) {
                    const unsigned int first = (row * (sides + 1U)) + segment;
                    const unsigned int second = first + sides + 1U;
                    mesh.indices.insert(mesh.indices.end(), {
                        first, second, first + 1U, first + 1U, second, second + 1U,
                    });
                }
            }
            return mesh;
        }
    };
} // namespace Engine
