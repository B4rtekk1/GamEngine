#include <Engine/ECS/Components/CameraComponent.h>
#include <Engine/Renderer/Geometry/Cube.h>
#include <Engine/Renderer/Geometry/Plane.h>
#include <Engine/Renderer/Geometry/Ramp.h>
#include <Engine/Renderer/Geometry/Sphere.h>

#include <cmath>

namespace {
bool near(const float a, const float b, const float epsilon = 0.001F) {
    return std::abs(a - b) <= epsilon;
}
}

int main() {
    using namespace Engine;

    const Mesh cube = Cube::createMesh();
    if (cube.empty() || cube.vertexCount() != 24 || cube.indexCount() != 36 ||
        cube.indices.back() >= cube.vertices.size()) return 1;
    if (!near(cube.vertices.front().position.x(), -0.5F) ||
        !near(cube.vertices.front().normal.z(), -1.0F)) return 2;

    const Mesh plane = Plane::createMesh();
    if (plane.vertexCount() != 4 || plane.indexCount() != 6 ||
        plane.indices != std::vector<std::uint32_t>{0, 1, 2, 2, 3, 0}) return 3;
    for (const auto& vertex : plane.vertices) {
        if (!near(vertex.position.y(), 0.0F) || !near(vertex.normal.y(), 1.0F)) return 4;
    }

    const Mesh ramp = Ramp::createMesh();
    if (ramp.vertexCount() != 18 || ramp.indexCount() != 30 ||
        Ramp::halfExtents().x() != 3.0F || Ramp::halfExtents().y() != 2.0F ||
        Ramp::halfExtents().z() != 2.0F) return 5;
    for (const auto index : ramp.indices) {
        if (index >= ramp.vertices.size()) return 6;
    }

    constexpr unsigned int rings = 4;
    constexpr unsigned int segments = 6;
    const Mesh sphere = Sphere::createMesh(rings, segments);
    if (sphere.vertexCount() != (rings + 1) * (segments + 1) ||
        sphere.indexCount() != rings * segments * 6) return 7;
    for (const auto& vertex : sphere.vertices) {
        if (!near(vertex.position.length(), 0.5F) || !near(vertex.normal.length(), 1.0F) ||
            vertex.texCoord.x() < 0.0F || vertex.texCoord.x() > 1.0F ||
            vertex.texCoord.y() < 0.0F || vertex.texCoord.y() > 1.0F) return 8;
    }

    CameraComponent camera;
    if (!camera.isPerspective() || !camera.isValid()) return 9;
    camera.setPerspective(200.0F, -1.0F, 0.01F);
    if (camera.fieldOfView != 179.0F || camera.nearClip != 0.0001F ||
        camera.farClip <= camera.nearClip || !camera.isValid()) return 10;
    const float previousAspect = camera.aspectRatio;
    camera.setAspectRatio(0.0F, 1080.0F);
    if (camera.aspectRatio != previousAspect) return 11;
    camera.setOrthographic(-5.0F, 2.0F, 1.0F);
    camera.setAspectRatio(1920.0F, 1080.0F);
    if (!camera.isOrthographic() || camera.orthographicSize != 0.0001F ||
        camera.nearClip != 2.0F || camera.farClip <= camera.nearClip ||
        !near(camera.aspectRatio, 16.0F / 9.0F) || !camera.isValid()) return 12;

    return 0;
}