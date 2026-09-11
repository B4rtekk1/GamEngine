#include "Engine/Renderer/Geometry/Meshlet.h"

#include "Engine/Renderer/Geometry/Mesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Engine {
namespace {
[[nodiscard]] float dot(const Vec3& left, const Vec3& right) noexcept {
    return left.x() * right.x() + left.y() * right.y() + left.z() * right.z();
}

void finish_meshlet(Mesh& mesh, const std::uint32_t firstVertex,
                    const std::uint32_t firstTriangle, const std::uint32_t material) {
    Meshlet result{
        .vertexOffset = firstVertex,
        .vertexCount = static_cast<std::uint32_t>(mesh.meshletVertices.size()) - firstVertex,
        .triangleOffset = firstTriangle,
        .triangleCount = static_cast<std::uint32_t>(mesh.meshletTriangles.size()) - firstTriangle,
        .materialIndex = material,
    };

    Vec3 center{};
    for (std::uint32_t index = 0; index < result.vertexCount; ++index)
        center += mesh.vertices[mesh.meshletVertices[result.vertexOffset + index]].position;
    center *= 1.0F / static_cast<float>(result.vertexCount);
    result.center = center;
    for (std::uint32_t index = 0; index < result.vertexCount; ++index) {
        const float distance = (mesh.vertices[mesh.meshletVertices[result.vertexOffset + index]].position - center).length();
        result.radius = std::max(result.radius, distance);
    }

    Vec3 axis{};
    for (std::uint32_t triangle = 0; triangle < result.triangleCount; ++triangle) {
        const std::uint32_t packed = mesh.meshletTriangles[result.triangleOffset + triangle];
        const auto a = static_cast<std::uint8_t>(packed & 0xffU);
        const auto b = static_cast<std::uint8_t>((packed >> 8U) & 0xffU);
        const auto c = static_cast<std::uint8_t>((packed >> 16U) & 0xffU);
        const Vec3& p0 = mesh.vertices[mesh.meshletVertices[result.vertexOffset + a]].position;
        const Vec3& p1 = mesh.vertices[mesh.meshletVertices[result.vertexOffset + b]].position;
        const Vec3& p2 = mesh.vertices[mesh.meshletVertices[result.vertexOffset + c]].position;
        const Vec3 normal = cross(p1 - p0, p2 - p0);
        if (normal.length() > 1.0e-7F) axis += normal.normalized();
    }
    if (axis.length() <= 1.0e-7F) {
        result.coneAxis = {0.0F, 0.0F, 1.0F};
        result.coneCutoff = -1.0F;
    } else {
        result.coneAxis = axis.normalized();
        result.coneCutoff = 1.0F;
        for (std::uint32_t triangle = 0; triangle < result.triangleCount; ++triangle) {
            const std::uint32_t packed = mesh.meshletTriangles[result.triangleOffset + triangle];
            const auto a = static_cast<std::uint8_t>(packed & 0xffU);
            const auto b = static_cast<std::uint8_t>((packed >> 8U) & 0xffU);
            const auto c = static_cast<std::uint8_t>((packed >> 16U) & 0xffU);
            const Vec3& p0 = mesh.vertices[mesh.meshletVertices[result.vertexOffset + a]].position;
            const Vec3& p1 = mesh.vertices[mesh.meshletVertices[result.vertexOffset + b]].position;
            const Vec3& p2 = mesh.vertices[mesh.meshletVertices[result.vertexOffset + c]].position;
            const Vec3 normal = cross(p1 - p0, p2 - p0);
            if (normal.length() > 1.0e-7F)
                result.coneCutoff = std::min(result.coneCutoff, dot(result.coneAxis, normal.normalized()));
        }
    }
    mesh.meshlets.push_back(result);
}
} // namespace

bool build_meshlets(Mesh& mesh, const MeshletBuildOptions options) noexcept {
    if (options.maxVertices == 0 || options.maxVertices > MeshletBuildOptions::MaxVertices ||
        options.maxTriangles == 0 || options.maxTriangles > MeshletBuildOptions::MaxTriangles ||
        mesh.indices.empty() || mesh.indices.size() % 3U != 0U) return false;
    for (const std::uint32_t index : mesh.indices) if (index >= mesh.vertices.size()) return false;

    mesh.meshlets.clear();
    mesh.meshletVertices.clear();
    mesh.meshletTriangles.clear();
    mesh.meshlets.reserve((mesh.indices.size() / 3U + options.maxTriangles - 1U) / options.maxTriangles);

    std::array<std::uint32_t, MeshletBuildOptions::MaxVertices> localVertices{};
    std::uint32_t localCount = 0;
    std::uint32_t vertexStart = 0;
    std::uint32_t triangleStart = 0;
    std::uint32_t material = 0;
    bool active = false;
    for (std::size_t source = 0; source < mesh.indices.size(); source += 3U) {
        const std::array<std::uint32_t, 3> triangle{
            mesh.indices[source], mesh.indices[source + 1U], mesh.indices[source + 2U]};
        const std::uint32_t triangleMaterial = mesh.vertices[triangle[0]].materialIndex;
        if (mesh.vertices[triangle[1]].materialIndex != triangleMaterial ||
            mesh.vertices[triangle[2]].materialIndex != triangleMaterial) return false;
        std::uint32_t additional = 0;
        for (const std::uint32_t index : triangle) {
            if (std::find(localVertices.begin(), localVertices.begin() + localCount, index) == localVertices.begin() + localCount)
                ++additional;
        }
        const bool needsFlush = active && (triangleMaterial != material ||
            localCount + additional > options.maxVertices ||
            mesh.meshletTriangles.size() - triangleStart >= options.maxTriangles);
        if (needsFlush) {
            finish_meshlet(mesh, vertexStart, triangleStart, material);
            localCount = 0;
            vertexStart = static_cast<std::uint32_t>(mesh.meshletVertices.size());
            triangleStart = static_cast<std::uint32_t>(mesh.meshletTriangles.size());
            active = false;
        }
        if (!active) { material = triangleMaterial; active = true; }
        std::uint32_t local[3]{};
        for (std::size_t corner = 0; corner < triangle.size(); ++corner) {
            const auto found = std::find(localVertices.begin(), localVertices.begin() + localCount, triangle[corner]);
            if (found == localVertices.begin() + localCount) {
                localVertices[localCount] = triangle[corner];
                mesh.meshletVertices.push_back(triangle[corner]);
                local[corner] = localCount++;
            } else local[corner] = static_cast<std::uint32_t>(found - localVertices.begin());
        }
        mesh.meshletTriangles.push_back(local[0] | (local[1] << 8U) | (local[2] << 16U));
    }
    if (active) finish_meshlet(mesh, vertexStart, triangleStart, material);
    return true;
}
} // namespace Engine
