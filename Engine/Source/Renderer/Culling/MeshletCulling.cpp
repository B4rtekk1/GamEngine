#include "Engine/Renderer/Culling/MeshletCulling.h"

#include <limits>

namespace Engine::Culling {
bool appendMeshletPayload(const Mesh& mesh, const std::uint32_t firstVertex,
                          std::vector<GpuMeshlet>& destinationMeshlets,
                          std::vector<std::uint32_t>& destinationVertices,
                          std::vector<std::uint32_t>& destinationTriangles) noexcept {
    if (mesh.meshlets.empty() || mesh.meshletVertices.empty() || mesh.meshletTriangles.empty() ||
        mesh.vertices.size() > std::numeric_limits<std::uint32_t>::max() - firstVertex ||
        destinationVertices.size() > std::numeric_limits<std::uint32_t>::max() ||
        destinationTriangles.size() > std::numeric_limits<std::uint32_t>::max()) return false;

    const std::uint32_t vertexBase = static_cast<std::uint32_t>(destinationVertices.size());
    const std::uint32_t triangleBase = static_cast<std::uint32_t>(destinationTriangles.size());
    for (const std::uint32_t vertex : mesh.meshletVertices) {
        if (vertex >= mesh.vertices.size()) return false;
        destinationVertices.push_back(firstVertex + vertex);
    }
    destinationTriangles.insert(destinationTriangles.end(), mesh.meshletTriangles.begin(), mesh.meshletTriangles.end());
    for (const Meshlet& source : mesh.meshlets) {
        if (source.vertexCount == 0 || source.vertexCount > MeshletBuildOptions::MaxVertices ||
            source.triangleCount == 0 || source.triangleCount > MeshletBuildOptions::MaxTriangles ||
            source.vertexOffset > mesh.meshletVertices.size() ||
            source.vertexCount > mesh.meshletVertices.size() - source.vertexOffset ||
            source.triangleOffset > mesh.meshletTriangles.size() ||
            source.triangleCount > mesh.meshletTriangles.size() - source.triangleOffset) return false;
        destinationMeshlets.push_back({
            .range = {vertexBase + source.vertexOffset, source.vertexCount,
                      triangleBase + source.triangleOffset, source.triangleCount},
            .bounds = {source.center.x(), source.center.y(), source.center.z(), source.radius},
            .cone = {source.coneAxis.x(), source.coneAxis.y(), source.coneAxis.z(), source.coneCutoff},
            .material = {source.materialIndex, 0U, 0U, 0U},
        });
    }
    return true;
}
} // namespace Engine::Culling
