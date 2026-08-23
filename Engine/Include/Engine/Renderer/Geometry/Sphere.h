#pragma once

#include "Engine/Renderer/Geometry/Mesh.h"

#include <cmath>

namespace Engine {

class Sphere final {
public:
    [[nodiscard]] static Mesh createMesh(const unsigned int rings = 32, const unsigned int segments = 48) {
        Mesh mesh;
        constexpr float pi = 3.14159265358979323846f;
        mesh.vertices.reserve((rings + 1) * (segments + 1));
        mesh.indices.reserve(rings * segments * 6);

        for (unsigned int ring = 0; ring <= rings; ++ring) {
            const float v = static_cast<float>(ring) / static_cast<float>(rings);
            const float phi = v * pi;
            for (unsigned int segment = 0; segment <= segments; ++segment) {
                const float u = static_cast<float>(segment) / static_cast<float>(segments);
                const float theta = u * pi * 2.0f;
                const Vec3 normal{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
                mesh.vertices.push_back({.position = normal * 0.5f, .color = {0.35f, 0.65f, 0.95f},
                                         .texCoord = {u, v}, .normal = normal});
            }
        }

        for (unsigned int ring = 0; ring < rings; ++ring) {
            for (unsigned int segment = 0; segment < segments; ++segment) {
                const unsigned int first = ring * (segments + 1) + segment;
                const unsigned int second = first + segments + 1;
                mesh.indices.insert(mesh.indices.end(), {first, second, first + 1, first + 1, second, second + 1});
            }
        }
        return mesh;
    }
};

} // namespace Engine
