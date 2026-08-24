#include <Engine/ECS/Components/CameraComponent.h>
#include <Engine/Renderer/Geometry/Cube.h>
#include <Engine/Renderer/Geometry/Plane.h>
#include <Engine/Renderer/Geometry/Ramp.h>
#include <Engine/Renderer/Geometry/Sphere.h>

#include <cmath>

namespace {
bool near(const float a, const float b, const float epsilon = 0.001f) {
    return std::abs(a - b) <= epsilon;
}
}

int main() {
    using namespace Engine;

    const Mesh cube = Cube::createMesh();
    if (cube.empty() || cube.vertexCount() != 24 || cube.indexCount() != 36 ||
        cube.indices.back() >= cube.vertices.size()) return 1;
    if (!near(cube.vertices.front().position.x(), -0.5f) ||
        !near(cube.vertices.front().normal.z(), -1.0f)) return 2;

    const Mesh plane = Plane::createMesh();
    if (plane.vertexCount() != 4 || plane.indexCount() != 6 ||
        plane.indices != std::vector<std::uint32_t>{0, 1, 2, 2, 3, 0}) return 3;
    for (const auto& vertex : plane.vertices) {
        if (!near(vertex.position.y(), 0.0f) || !near(vertex.normal.y(), 1.0f)) return 4;
    }

    const Mesh ramp = Ramp::createMesh();
    if (ramp.vertexCount() != 18 || ramp.indexCount() != 30 ||
        Ramp::halfExtents().x() != 3.0f || Ramp::halfExtents().y() != 2.0f ||
        Ramp::halfExtents().z() != 2.0f) return 5;
    for (const auto index : ramp.indices) {
        if (index >= ramp.vertices.size()) return 6;
    }

    constexpr unsigned int rings = 4;
    constexpr unsigned int segments = 6;
    const Mesh sphere = Sphere::createMesh(rings, segments);
    if (sphere.vertexCount() != (rings + 1) * (segments + 1) ||
        sphere.indexCount() != rings * segments * 6) return 7;
    for (const auto& vertex : sphere.vertices) {
        if (!near(vertex.position.length(), 0.5f) || !near(vertex.normal.length(), 1.0f) ||
            vertex.texCoord.x() < 0.0f || vertex.texCoord.x() > 1.0f ||
            vertex.texCoord.y() < 0.0f || vertex.texCoord.y() > 1.0f) return 8;
    }

    CameraComponent camera;
    if (!camera.isPerspective() || !camera.isValid()) return 9;
    camera.setPerspective(200.0f, -1.0f, 0.01f);
    if (camera.fieldOfView != 179.0f || camera.nearClip != 0.0001f ||
        camera.farClip <= camera.nearClip || !camera.isValid()) return 10;
    const float previousAspect = camera.aspectRatio;
    camera.setAspectRatio(0.0f, 1080.0f);
    if (camera.aspectRatio != previousAspect) return 11;
    camera.setOrthographic(-5.0f, 2.0f, 1.0f);
    camera.setAspectRatio(1920.0f, 1080.0f);
    if (!camera.isOrthographic() || camera.orthographicSize != 0.0001f ||
        camera.nearClip != 2.0f || camera.farClip <= camera.nearClip ||
        !near(camera.aspectRatio, 16.0f / 9.0f) || !camera.isValid()) return 12;

    return 0;
}
