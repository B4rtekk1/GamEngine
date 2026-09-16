#include "Engine/Renderer/Geometry/Meshlet.h"

#include "Engine/Renderer/Geometry/Mesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>

namespace Engine {
    namespace {
        [[nodiscard]] float dot(const Vec3 &left, const Vec3 &right) noexcept {
            return left.x() * right.x() + left.y() * right.y() + left.z() * right.z();
        }

        void finish_meshlet(Mesh &mesh, const std::uint32_t firstVertex,
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
                const float distance = (mesh.vertices[mesh.meshletVertices[result.vertexOffset + index]].position -
                                        center).length();
                result.radius = std::max(result.radius, distance);
            }

            Vec3 axis{};
            for (std::uint32_t triangle = 0; triangle < result.triangleCount; ++triangle) {
                const std::uint32_t packed = mesh.meshletTriangles[result.triangleOffset + triangle];
                const auto a = static_cast<std::uint8_t>(packed & 0xffU);
                const auto b = static_cast<std::uint8_t>((packed >> 8U) & 0xffU);
                const auto c = static_cast<std::uint8_t>((packed >> 16U) & 0xffU);
                const Vec3 &p0 = mesh.vertices[mesh.meshletVertices[result.vertexOffset + a]].position;
                const Vec3 &p1 = mesh.vertices[mesh.meshletVertices[result.vertexOffset + b]].position;
                const Vec3 &p2 = mesh.vertices[mesh.meshletVertices[result.vertexOffset + c]].position;
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
                    const Vec3 &p0 = mesh.vertices[mesh.meshletVertices[result.vertexOffset + a]].position;
                    const Vec3 &p1 = mesh.vertices[mesh.meshletVertices[result.vertexOffset + b]].position;
                    const Vec3 &p2 = mesh.vertices[mesh.meshletVertices[result.vertexOffset + c]].position;
                    const Vec3 normal = cross(p1 - p0, p2 - p0);
                    if (normal.length() > 1.0e-7F)
                        result.coneCutoff = std::min(result.coneCutoff, dot(result.coneAxis, normal.normalized()));
                }
            }
            mesh.meshlets.push_back(result);
        }

        [[nodiscard]] AABB bounds_for_indices(const Mesh &mesh, const std::uint32_t firstIndex,
                                              const std::uint32_t indexCount) noexcept {
            AABB bounds{
                .min = Vec3{
                    std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                    std::numeric_limits<float>::max()
                },
                .max = Vec3{
                    std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                    std::numeric_limits<float>::lowest()
                },
            };
            for (std::uint32_t offset = 0; offset < indexCount; ++offset) {
                const Vec3 &position = mesh.vertices[mesh.indices[firstIndex + offset]].position;
                bounds.min.setX(std::min(bounds.min.x(), position.x()));
                bounds.min.setY(std::min(bounds.min.y(), position.y()));
                bounds.min.setZ(std::min(bounds.min.z(), position.z()));
                bounds.max.setX(std::max(bounds.max.x(), position.x()));
                bounds.max.setY(std::max(bounds.max.y(), position.y()));
                bounds.max.setZ(std::max(bounds.max.z(), position.z()));
            }
            return bounds;
        }

        [[nodiscard]] std::uint32_t morton_code(const Vec3 &point, const AABB &bounds) noexcept {
            const auto quantize = [](const float value, const float minimum, const float maximum) {
                const float extent = maximum - minimum;
                if (extent <= 1.0e-7F) return 0U;
                return static_cast<std::uint32_t>(std::clamp((value - minimum) / extent, 0.0F, 1.0F) * 1023.0F);
            };
            const std::uint32_t x = quantize(point.x(), bounds.min.x(), bounds.max.x());
            const std::uint32_t y = quantize(point.y(), bounds.min.y(), bounds.max.y());
            const std::uint32_t z = quantize(point.z(), bounds.min.z(), bounds.max.z());
            std::uint32_t result = 0;
            for (std::uint32_t bit = 0; bit < 10U; ++bit)
                result |= ((x >> bit) & 1U) << (3U * bit) | ((y >> bit) & 1U) << (3U * bit + 1U) |
                        ((z >> bit) & 1U) << (3U * bit + 2U);
            return result;
        }
    } // namespace

    void subdivide_render_sections(Mesh &mesh, const std::uint32_t targetMeshlets) noexcept {
        if (targetMeshlets == 0 || mesh.renderSections.empty()) return;
        constexpr std::uint32_t maxClusterTriangles = MeshletBuildOptions::MaxTriangles;
        if (targetMeshlets > std::numeric_limits<std::uint32_t>::max() / maxClusterTriangles / 3U) return;
        const std::uint32_t targetIndices = targetMeshlets * maxClusterTriangles * 3U;
        std::vector<Mesh::RenderSection> sections;
        sections.reserve(mesh.renderSections.size());
        for (const Mesh::RenderSection &section: mesh.renderSections) {
            if (section.indexCount == 0 || section.indexCount % 3U != 0U ||
                section.firstIndex > mesh.indices.size() || section.indexCount > mesh.indices.size() - section.
                firstIndex ||
                section.indexCount <= targetIndices) {
                sections.push_back(section);
                continue;
            }
            const AABB primitiveBounds = bounds_for_indices(mesh, section.firstIndex, section.indexCount);
            std::vector<std::uint32_t> triangleOrder(section.indexCount / 3U);
            std::iota(triangleOrder.begin(), triangleOrder.end(), 0U);
            std::stable_sort(triangleOrder.begin(), triangleOrder.end(),
                             [&](const std::uint32_t left, const std::uint32_t right) {
                                 const auto centroid = [&](const std::uint32_t triangle) {
                                     const std::uint32_t index = section.firstIndex + triangle * 3U;
                                     return (mesh.vertices[mesh.indices[index]].position + mesh.vertices[mesh.indices[
                                                 index + 1U]].position +
                                             mesh.vertices[mesh.indices[index + 2U]].position) * (1.0F / 3.0F);
                                 };
                                 return morton_code(centroid(left), primitiveBounds) < morton_code(
                                            centroid(right), primitiveBounds);
                             });
            std::vector<std::uint32_t> reordered;
            reordered.reserve(section.indexCount);
            for (const std::uint32_t triangle: triangleOrder) {
                const std::uint32_t index = section.firstIndex + triangle * 3U;
                reordered.insert(reordered.end(), mesh.indices.begin() + index, mesh.indices.begin() + index + 3U);
            }
            std::copy(reordered.begin(), reordered.end(), mesh.indices.begin() + section.firstIndex);
            for (std::uint32_t first = section.firstIndex; first < section.firstIndex + section.indexCount;
                 first += targetIndices) {
                const std::uint32_t count = std::min(targetIndices, section.firstIndex + section.indexCount - first);
                sections.push_back({
                    .firstIndex = first, .indexCount = count, .materialIndex = section.materialIndex,
                    .localBounds = bounds_for_indices(mesh, first, count)
                });
            }
        }
        mesh.renderSections = std::move(sections);
    }

    bool build_meshlets(Mesh &mesh, const MeshletBuildOptions options) noexcept {
        if (options.maxVertices == 0 || options.maxVertices > MeshletBuildOptions::MaxVertices ||
            options.maxTriangles == 0 || options.maxTriangles > MeshletBuildOptions::MaxTriangles ||
            mesh.indices.empty() || mesh.indices.size() % 3U != 0U)
            return false;
        for (const std::uint32_t index: mesh.indices) if (index >= mesh.vertices.size()) return false;

        mesh.meshlets.clear();
        mesh.meshletVertices.clear();
        mesh.meshletTriangles.clear();
        mesh.meshlets.reserve((mesh.indices.size() / 3U + options.maxTriangles - 1U) / options.maxTriangles);

        const auto buildRange = [&](const std::uint32_t firstIndex, const std::uint32_t indexCount) -> bool {
            if (indexCount == 0 || indexCount % 3U != 0U || firstIndex > mesh.indices.size() ||
                indexCount > mesh.indices.size() - firstIndex)
                return false;
            std::array<std::uint32_t, MeshletBuildOptions::MaxVertices> localVertices{};
            std::uint32_t localCount = 0;
            std::uint32_t vertexStart = static_cast<std::uint32_t>(mesh.meshletVertices.size());
            std::uint32_t triangleStart = static_cast<std::uint32_t>(mesh.meshletTriangles.size());
            std::uint32_t material = 0;
            bool active = false;
            for (std::size_t source = firstIndex; source < static_cast<std::size_t>(firstIndex) + indexCount;
                 source += 3U) {
                const std::array<std::uint32_t, 3> triangle{
                    mesh.indices[source], mesh.indices[source + 1U], mesh.indices[source + 2U]
                };
                const std::uint32_t triangleMaterial = mesh.vertices[triangle[0]].materialIndex;
                if (mesh.vertices[triangle[1]].materialIndex != triangleMaterial ||
                    mesh.vertices[triangle[2]].materialIndex != triangleMaterial)
                    return false;
                std::uint32_t additional = 0;
                for (const std::uint32_t index: triangle) {
                    if (std::find(localVertices.begin(), localVertices.begin() + localCount, index) == localVertices.
                        begin() + localCount)
                        ++additional;
                }
                const bool needsFlush = active && (triangleMaterial != material ||
                                                   localCount + additional > options.maxVertices ||
                                                   mesh.meshletTriangles.size() - triangleStart >= options.
                                                   maxTriangles);
                if (needsFlush) {
                    finish_meshlet(mesh, vertexStart, triangleStart, material);
                    localCount = 0;
                    vertexStart = static_cast<std::uint32_t>(mesh.meshletVertices.size());
                    triangleStart = static_cast<std::uint32_t>(mesh.meshletTriangles.size());
                    active = false;
                }
                if (!active) {
                    material = triangleMaterial;
                    active = true;
                }
                std::uint32_t local[3]{};
                for (std::size_t corner = 0; corner < triangle.size(); ++corner) {
                    const auto found = std::find(localVertices.begin(), localVertices.begin() + localCount,
                                                 triangle[corner]);
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
        };
        if (mesh.renderSections.empty()) return buildRange(0, static_cast<std::uint32_t>(mesh.indices.size()));
        for (Mesh::RenderSection &section: mesh.renderSections) {
            section.firstMeshlet = static_cast<std::uint32_t>(mesh.meshlets.size());
            if (!buildRange(section.firstIndex, section.indexCount)) return false;
            section.meshletCount = static_cast<std::uint32_t>(mesh.meshlets.size()) - section.firstMeshlet;
        }
        return true;
    }
} // namespace Engine
