#include "Engine/Renderer/Culling/MeshletCulling.h"

#include <limits>
#include <cmath>
#include <glm/common.hpp>
#include <glm/geometric.hpp>

namespace Engine::Culling {
namespace {
float displacementExpansion(const PBRMaterial& material) {
    if (material.displacementTexture < 0) return 0.0F;
    const float d0 = material.displacementOffset;
    const float d1 = material.displacementOffset + material.displacementScale;
    return std::max(std::abs(d0), std::abs(d1));
}
}

bool appendMeshletPayload(const Mesh& mesh, const std::uint32_t firstVertex,
                          std::vector<GpuMeshlet>& destinationMeshlets,
                          std::vector<std::uint32_t>& destinationVertices,
                          std::vector<std::uint32_t>& destinationTriangles) {
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
        const float expansion = source.materialIndex < mesh.materials.size()
            ? displacementExpansion(mesh.materials[source.materialIndex]) : 0.0F;
        destinationMeshlets.push_back({
            .range = {vertexBase + source.vertexOffset, source.vertexCount,
                      triangleBase + source.triangleOffset, source.triangleCount},
            .bounds = {source.center.x(), source.center.y(), source.center.z(), source.radius + expansion},
            .cone = {source.coneAxis.x(), source.coneAxis.y(), source.coneAxis.z(), source.coneCutoff},
            .material = {source.materialIndex, 0U, 0U, 0U},
        });
    }
    return true;
}

bool appendMeshletClusterPayload(const Mesh& mesh, const std::uint32_t firstMeshlet,
                                 std::vector<GpuMeshletCluster>& destination) {
    if (mesh.meshletClusters.empty() ||
        destination.size() > std::numeric_limits<std::uint32_t>::max() - mesh.meshletClusters.size()) return false;
    const std::uint32_t clusterBase = static_cast<std::uint32_t>(destination.size());
    float maxDisplacement = 0.0F;
    for (const PBRMaterial& material : mesh.materials)
        maxDisplacement = std::max(maxDisplacement, displacementExpansion(material));
    for (const MeshletClusterNode& node : mesh.meshletClusters) {
        if ((node.childCount == 0u && node.meshletCount == 0u) ||
            (node.childCount != 0u && (node.firstChild > mesh.meshletClusters.size() ||
             node.childCount > mesh.meshletClusters.size() - node.firstChild)) ||
            (node.meshletCount != 0u && (node.firstMeshlet > mesh.meshlets.size() ||
             node.meshletCount > mesh.meshlets.size() - node.firstMeshlet))) return false;

        // Union child normal cones.  A node cone is only emitted if it covers
        // every descendant cone; otherwise it is disabled rather than risking
        // a false occlusion/backface reject. Children precede parents in the
        // baked breadth-first stream, so their already-conservative unions
        // are available here.
        glm::vec3 axisSum{};
        bool validCone = true;
        const auto includeCone = [&](const glm::vec4 cone) {
            if (cone.w <= -1.0F || glm::dot(glm::vec3{cone}, glm::vec3{cone}) < 1.0e-12F) {
                validCone = false;
                return;
            }
            axisSum += glm::normalize(glm::vec3{cone});
        };
        if (node.childCount != 0u) {
            for (std::uint32_t child = 0; child < node.childCount; ++child)
                includeCone(destination[clusterBase + node.firstChild + child].cone);
        } else {
            for (std::uint32_t meshlet = 0; meshlet < node.meshletCount; ++meshlet) {
                const Meshlet& source = mesh.meshlets[node.firstMeshlet + meshlet];
                includeCone(glm::vec4{source.coneAxis.x(), source.coneAxis.y(), source.coneAxis.z(),
                                      source.coneCutoff});
            }
        }
        glm::vec4 cone{0.0F, 0.0F, 0.0F, -2.0F};
        if (validCone && glm::dot(axisSum, axisSum) > 1.0e-12F) {
            const glm::vec3 axis = glm::normalize(axisSum);
            float halfAngle = 0.0F;
            const auto includeExtent = [&](const glm::vec4 source) {
                const float axisAngle = std::acos(glm::clamp(glm::dot(axis, glm::normalize(glm::vec3{source})), -1.0F, 1.0F));
                halfAngle = std::max(halfAngle, axisAngle + std::acos(glm::clamp(source.w, -1.0F, 1.0F)));
            };
            if (node.childCount != 0u) {
                for (std::uint32_t child = 0; child < node.childCount; ++child)
                    includeExtent(destination[clusterBase + node.firstChild + child].cone);
            } else {
                for (std::uint32_t meshlet = 0; meshlet < node.meshletCount; ++meshlet) {
                    const Meshlet& source = mesh.meshlets[node.firstMeshlet + meshlet];
                    includeExtent(glm::vec4{source.coneAxis.x(), source.coneAxis.y(), source.coneAxis.z(), source.coneCutoff});
                }
            }
            if (halfAngle < 3.14159265F) cone = glm::vec4{axis, std::cos(halfAngle)};
        }
        destination.push_back({
            .bounds = {node.center.x(), node.center.y(), node.center.z(), node.radius + maxDisplacement},
            .cone = cone,
            .range = {clusterBase + node.firstChild, node.childCount,
                      firstMeshlet + node.firstMeshlet, node.meshletCount},
        });
    }
    return true;
}
} // namespace Engine::Culling
