#include "Engine/Assets/Gmesh.h"

#include "Engine/Assets/Gtex.h"
#include "GlbLoader.h"
#include "Engine/Assets/TextureCooker.h"
#include "Engine/Renderer/Geometry/Mesh.h"
#include "Engine/Renderer/Geometry/Meshlet.h"

#include <array>
#include <fstream>
#include <limits>
#include <ranges>
#include <string>
#include <type_traits>

namespace Engine::Assets {
    namespace {
        constexpr std::array<char, 8> magic{'G', 'M', 'E', 'S', 'H', '\0', '\0', '\0'};
        constexpr std::uint32_t version = 5;
        constexpr std::uint32_t maxElements = 100'000'000;

        struct HeaderV1 {
            std::array<char, 8> magic;
            std::uint32_t version;
            std::uint32_t vertices;
            std::uint32_t indices;
            std::uint32_t materials;
            std::uint32_t images;
        };

        struct FilePrefix final {
            std::array<char, 8> magic;
            std::uint32_t version;
        };

        struct Header final {
            std::array<char, 8> magic;
            std::uint32_t version;
            std::uint32_t vertices;
            std::uint32_t indices;
            std::uint32_t meshlets;
            std::uint32_t meshletVertices;
            std::uint32_t meshletTriangles;
            std::uint32_t materials;
            std::uint32_t images;
            std::uint32_t renderSections;
            std::uint32_t meshletClusters;
            std::uint32_t meshletClusterRoot;
            AABB localBounds;
        };

        struct RenderSectionV3 final {
            std::uint32_t firstIndex, indexCount, firstMeshlet, meshletCount, materialIndex;
            AABB localBounds{};
        };

        static_assert(std::is_trivially_copyable_v<Vertex>);
        static_assert(std::is_trivially_copyable_v<PBRMaterial>);
        static_assert(std::is_trivially_copyable_v<Meshlet>);
        static_assert(std::is_trivially_copyable_v<Mesh::RenderSection>);
        static_assert(std::is_trivially_copyable_v<MeshletClusterNode>);

        template<class T>
        bool write(std::ofstream &file, const T &value) {
            file.write(reinterpret_cast<const char *>(&value), sizeof value);
            return static_cast<bool>(file);
        }

        template<class T>
        bool read(std::ifstream &file, T &value) {
            file.read(reinterpret_cast<char *>(&value), sizeof value);
            return static_cast<bool>(file);
        }

        template<class T>
        bool write_vector(std::ofstream &file, const std::vector<T> &values) {
            if (values.empty()) return true;
            file.write(reinterpret_cast<const char *>(values.data()),
                       static_cast<std::streamsize>(values.size() * sizeof(T)));
            return static_cast<bool>(file);
        }

        template<class T>
        bool read_vector(std::ifstream &file, std::vector<T> &values) {
            if (values.empty()) return true;
            file.read(reinterpret_cast<char *>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(T)));
            return static_cast<bool>(file);
        }

        std::filesystem::path texture_path(const std::filesystem::path &mesh, const std::size_t index) {
            return mesh.parent_path() / (mesh.stem().string() + ".image" + std::to_string(index) + ".gtex");
        }

        [[nodiscard]] bool valid_meshlets(const Mesh &mesh) {
            for (const Meshlet &meshlet: mesh.meshlets) {
                if (meshlet.vertexCount == 0 || meshlet.vertexCount > MeshletBuildOptions::MaxVertices ||
                    meshlet.triangleCount == 0 || meshlet.triangleCount > MeshletBuildOptions::MaxTriangles ||
                    meshlet.vertexOffset > mesh.meshletVertices.size() ||
                    meshlet.vertexCount > mesh.meshletVertices.size() - meshlet.vertexOffset ||
                    meshlet.triangleOffset > mesh.meshletTriangles.size() ||
                    meshlet.triangleCount > mesh.meshletTriangles.size() - meshlet.triangleOffset)
                    return false;
                for (std::uint32_t index = 0; index < meshlet.vertexCount; ++index)
                    if (mesh.meshletVertices[meshlet.vertexOffset + index] >= mesh.vertices.size()) return false;
                for (std::uint32_t index = 0; index < meshlet.triangleCount; ++index) {
                    const std::uint32_t triangle = mesh.meshletTriangles[meshlet.triangleOffset + index];
                    if ((triangle & 0xffU) >= meshlet.vertexCount || ((triangle >> 8U) & 0xffU) >= meshlet.vertexCount
                        ||
                        ((triangle >> 16U) & 0xffU) >= meshlet.vertexCount || (triangle >> 24U) != 0U)
                        return false;
                }
            }
            return !mesh.indices.empty() ? !mesh.meshlets.empty() : mesh.meshlets.empty();
        }

        [[nodiscard]] bool valid_render_sections(const Mesh &mesh) {
            for (const Mesh::RenderSection &section: mesh.renderSections) {
                if (section.indexCount == 0 || section.indexCount % 3U != 0U ||
                    section.firstIndex > mesh.indices.size() || section.indexCount > mesh.indices.size() - section.
                    firstIndex ||
                    section.firstMeshlet > mesh.meshlets.size() || section.meshletCount > mesh.meshlets.size() - section
                    .firstMeshlet ||
                    (!mesh.materials.empty() && section.materialIndex >= mesh.materials.size()) ||
                    section.meshletClusterRoot >= mesh.meshletClusters.size())
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool valid_cluster_hierarchy(const Mesh& mesh) {
            if (mesh.meshletClusters.empty()) return false;
            for (const MeshletClusterNode& node : mesh.meshletClusters) {
                if (node.radius < 0.0F || node.geometricError < 0.0F ||
                    (node.childCount == 0U && (node.meshletCount == 0U || node.firstMeshlet > mesh.meshlets.size() ||
                     node.meshletCount > mesh.meshlets.size() - node.firstMeshlet)) ||
                    (node.childCount != 0U && (node.firstChild > mesh.meshletClusters.size() ||
                     node.childCount > mesh.meshletClusters.size() - node.firstChild))) return false;
            }
            return mesh.renderSections.empty() ? mesh.meshletClusterRoot < mesh.meshletClusters.size() : true;
        }
    }

    bool save_gmesh(const std::filesystem::path &path, const Mesh &mesh) {
        Mesh cooked = mesh;
        cooked.recalculateLocalBounds();
        subdivide_render_sections(cooked);
        if (!build_meshlets(cooked)) return false;
        if (cooked.vertices.size() > maxElements || cooked.indices.size() > maxElements || cooked.meshlets.size() >
            maxElements || cooked.meshletVertices.size() > maxElements || cooked.meshletTriangles.size() > maxElements
            || cooked.materials.size() > maxElements || cooked.images.size() > maxElements ||
            cooked.renderSections.size() > maxElements || cooked.meshletClusters.size() > maxElements)
            return false;
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        const Header header{
            magic, version, static_cast<std::uint32_t>(cooked.vertices.size()),
            static_cast<std::uint32_t>(cooked.indices.size()), static_cast<std::uint32_t>(cooked.meshlets.size()),
            static_cast<std::uint32_t>(cooked.meshletVertices.size()),
            static_cast<std::uint32_t>(cooked.meshletTriangles.size()),
            static_cast<std::uint32_t>(cooked.materials.size()), static_cast<std::uint32_t>(cooked.images.size()),
            static_cast<std::uint32_t>(cooked.renderSections.size()),
            static_cast<std::uint32_t>(cooked.meshletClusters.size()), cooked.meshletClusterRoot, cooked.localBounds
        };
        if (!file || !write(file, header) || !write_vector(file, cooked.vertices) || !write_vector(file, cooked.indices)
            || !write_vector(file, cooked.meshlets) || !write_vector(file, cooked.meshletVertices) || !
            write_vector(file, cooked.meshletTriangles) || !write_vector(file, cooked.materials) ||
            !write_vector(file, cooked.renderSections) || !write_vector(file, cooked.meshletClusters))
            return false;
        for (const auto &image: cooked.images) {
            const auto pathText = image.cookedPath.generic_string();
            const auto length = static_cast<std::uint32_t>(pathText.size());
            if (!write(file, length)) return false;
            file.write(pathText.data(), length);
            if (!file) return false;
        }
        return true;
    }

    std::shared_ptr<const Mesh> load_gmesh(const std::filesystem::path &path) {
        std::ifstream file(path, std::ios::binary);
        FilePrefix prefix{};
        if (!file || !read(file, prefix) || prefix.magic != magic ||
            (prefix.version < 1 || prefix.version > version))
            return {};
        file.seekg(0);
        Mesh mesh;
        if (prefix.version == version) {
            Header header{};
            if (!read(file, header) || header.vertices > maxElements || header.indices > maxElements || header.meshlets
                > maxElements || header.meshletVertices > maxElements || header.meshletTriangles > maxElements || header
                .materials > maxElements || header.images > maxElements || header.renderSections > maxElements ||
                header.meshletClusters > maxElements)
                return {};
            mesh.vertices.resize(header.vertices);
            mesh.indices.resize(header.indices);
            mesh.meshlets.resize(header.meshlets);
            mesh.meshletVertices.resize(header.meshletVertices);
            mesh.meshletTriangles.resize(header.meshletTriangles);
            mesh.materials.resize(header.materials);
            mesh.images.resize(header.images);
            mesh.renderSections.resize(header.renderSections);
            mesh.meshletClusters.resize(header.meshletClusters);
            mesh.meshletClusterRoot = header.meshletClusterRoot;
            mesh.localBounds = header.localBounds;
            if (!read_vector(file, mesh.vertices) || !read_vector(file, mesh.indices) || !
                read_vector(file, mesh.meshlets) || !read_vector(file, mesh.meshletVertices) || !
                read_vector(file, mesh.meshletTriangles) || !read_vector(file, mesh.materials) ||
                !read_vector(file, mesh.renderSections) || !read_vector(file, mesh.meshletClusters))
                return {};
        } else if (prefix.version == 4) {
            struct HeaderV4 final {
                std::array<char, 8> magic;
                std::uint32_t version, vertices, indices, meshlets, meshletVertices, meshletTriangles,
                    materials, images, renderSections, meshletClusters, meshletClusterRoot;
            } header{};
            if (!read(file, header) || header.vertices > maxElements || header.indices > maxElements ||
                header.meshlets > maxElements || header.meshletVertices > maxElements ||
                header.meshletTriangles > maxElements || header.materials > maxElements ||
                header.images > maxElements || header.renderSections > maxElements || header.meshletClusters > maxElements)
                return {};
            mesh.vertices.resize(header.vertices); mesh.indices.resize(header.indices); mesh.meshlets.resize(header.meshlets);
            mesh.meshletVertices.resize(header.meshletVertices); mesh.meshletTriangles.resize(header.meshletTriangles);
            mesh.materials.resize(header.materials); mesh.images.resize(header.images);
            mesh.renderSections.resize(header.renderSections); mesh.meshletClusters.resize(header.meshletClusters);
            mesh.meshletClusterRoot = header.meshletClusterRoot;
            if (!read_vector(file, mesh.vertices) || !read_vector(file, mesh.indices) ||
                !read_vector(file, mesh.meshlets) || !read_vector(file, mesh.meshletVertices) ||
                !read_vector(file, mesh.meshletTriangles) || !read_vector(file, mesh.materials) ||
                !read_vector(file, mesh.renderSections) || !read_vector(file, mesh.meshletClusters)) return {};
        } else if (prefix.version == 3) {
            struct HeaderV3 final { std::array<char, 8> magic; std::uint32_t version, vertices, indices, meshlets,
                meshletVertices, meshletTriangles, materials, images, renderSections; } header{};
            if (!read(file, header) || header.vertices > maxElements || header.indices > maxElements || header.meshlets > maxElements ||
                header.meshletVertices > maxElements || header.meshletTriangles > maxElements || header.materials > maxElements ||
                header.images > maxElements || header.renderSections > maxElements) return {};
            mesh.vertices.resize(header.vertices); mesh.indices.resize(header.indices); mesh.meshlets.resize(header.meshlets);
            mesh.meshletVertices.resize(header.meshletVertices); mesh.meshletTriangles.resize(header.meshletTriangles);
            mesh.materials.resize(header.materials); mesh.images.resize(header.images);
            std::vector<RenderSectionV3> sections(header.renderSections);
            if (!read_vector(file, mesh.vertices) || !read_vector(file, mesh.indices) || !read_vector(file, mesh.meshlets) ||
                !read_vector(file, mesh.meshletVertices) || !read_vector(file, mesh.meshletTriangles) ||
                !read_vector(file, mesh.materials) || !read_vector(file, sections)) return {};
            mesh.renderSections.reserve(sections.size());
            for (const RenderSectionV3& section : sections)
                mesh.renderSections.push_back({section.firstIndex, section.indexCount, section.firstMeshlet,
                                               section.meshletCount, section.materialIndex, section.localBounds});
            if (!build_meshlet_cluster_hierarchy(mesh)) return {};
        } else if (prefix.version == 2) {
            struct HeaderV2 final {
                std::array<char, 8> magic;
                std::uint32_t version, vertices, indices, meshlets,
                        meshletVertices, meshletTriangles, materials, images;
            } header{};
            if (!read(file, header) || header.vertices > maxElements || header.indices > maxElements ||
                header.meshlets > maxElements || header.meshletVertices > maxElements ||
                header.meshletTriangles > maxElements || header.materials > maxElements || header.images > maxElements)
                return {};
            mesh.vertices.resize(header.vertices);
            mesh.indices.resize(header.indices);
            mesh.meshlets.resize(header.meshlets);
            mesh.meshletVertices.resize(header.meshletVertices);
            mesh.meshletTriangles.resize(header.meshletTriangles);
            mesh.materials.resize(header.materials);
            mesh.images.resize(header.images);
            if (!read_vector(file, mesh.vertices) || !read_vector(file, mesh.indices) || !read_vector(
                    file, mesh.meshlets) ||
                !read_vector(file, mesh.meshletVertices) || !read_vector(file, mesh.meshletTriangles) ||
                !read_vector(file, mesh.materials))
                return {};
        } else {
            HeaderV1 header{};
            if (!read(file, header) || header.vertices > maxElements || header.indices > maxElements || header.materials
                > maxElements || header.images > maxElements)
                return {};
            mesh.vertices.resize(header.vertices);
            mesh.indices.resize(header.indices);
            mesh.materials.resize(header.materials);
            mesh.images.resize(header.images);
            if (!read_vector(file, mesh.vertices) || !read_vector(file, mesh.indices) || !
                read_vector(file, mesh.materials) || !build_meshlets(mesh))
                return {};
        }
        for (auto &image: mesh.images) {
            std::uint32_t length{};
            if (!read(file, length) || length > 32'768) return {};
            std::string text(length, '\0');
            file.read(text.data(), length);
            if (!file) return {};
            image.cookedPath = text;
            auto gtex = load_gtex(path.parent_path() / image.cookedPath);
            if (!gtex) return {};
            image.width = gtex->width;
            image.height = gtex->height;
            image.gtex.emplace(std::move(*gtex));
        }
        if (mesh.empty() || !valid_meshlets(mesh)) return {};
        mesh.recalculateLocalBounds();
        if (mesh.renderSections.empty()) {
            AABB bounds{
                .min = Vec3{
                    std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                    std::numeric_limits<float>::max()
                },
                .max = Vec3{
                    std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                    std::numeric_limits<float>::lowest()
                }
            };
            for (const Vertex &vertex: mesh.vertices) {
                bounds.min.setX(std::min(bounds.min.x(), vertex.position.x()));
                bounds.min.setY(std::min(bounds.min.y(), vertex.position.y()));
                bounds.min.setZ(std::min(bounds.min.z(), vertex.position.z()));
                bounds.max.setX(std::max(bounds.max.x(), vertex.position.x()));
                bounds.max.setY(std::max(bounds.max.y(), vertex.position.y()));
                bounds.max.setZ(std::max(bounds.max.z(), vertex.position.z()));
            }
            mesh.renderSections.push_back({
                0, mesh.indexCount(), 0, static_cast<std::uint32_t>(mesh.meshlets.size()), 0, bounds
            });
        }
        if (mesh.meshletClusters.empty() && !build_meshlet_cluster_hierarchy(mesh)) return {};
        if (!valid_render_sections(mesh) || !valid_cluster_hierarchy(mesh)) return {};
        mesh.sourcePath = path;
        return std::make_shared<const Mesh>(std::move(mesh));
    }

    std::shared_ptr<Mesh> load_gmesh_geometry(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        FilePrefix prefix{};
        if (!file || !read(file, prefix) || prefix.magic != magic) return {};
        // Older versions do not have the same independently skippable payload
        // layout. Keep them loadable while current cooked assets use streaming.
        if (prefix.version != version) {
            const auto complete = load_gmesh(path);
            return complete ? std::make_shared<Mesh>(*complete) : nullptr;
        }
        file.seekg(0);
        Header header{};
        if (!read(file, header) || header.vertices > maxElements || header.indices > maxElements ||
            header.meshlets > maxElements || header.meshletVertices > maxElements ||
            header.meshletTriangles > maxElements || header.materials > maxElements ||
            header.images > maxElements || header.renderSections > maxElements ||
            header.meshletClusters > maxElements) return {};

        Mesh mesh;
        mesh.vertices.resize(header.vertices);
        mesh.indices.resize(header.indices);
        mesh.meshlets.resize(header.meshlets);
        mesh.meshletVertices.resize(header.meshletVertices);
        mesh.meshletTriangles.resize(header.meshletTriangles);
        mesh.meshletClusters.resize(header.meshletClusters);
        mesh.meshletClusterRoot = header.meshletClusterRoot;
        mesh.localBounds = header.localBounds;
        if (!read_vector(file, mesh.vertices) || !read_vector(file, mesh.indices) ||
            !read_vector(file, mesh.meshlets) || !read_vector(file, mesh.meshletVertices) ||
            !read_vector(file, mesh.meshletTriangles)) return {};

        const auto skipRecords = [&file](const std::uint32_t count, const std::size_t stride) {
            const auto bytes = static_cast<std::uint64_t>(count) * stride;
            if (bytes > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) return false;
            file.seekg(static_cast<std::streamoff>(bytes), std::ios::cur);
            return static_cast<bool>(file);
        };
        if (!skipRecords(header.materials, sizeof(PBRMaterial)) ||
            !skipRecords(header.renderSections, sizeof(Mesh::RenderSection)) ||
            !read_vector(file, mesh.meshletClusters) || mesh.empty() || !valid_meshlets(mesh) ||
            !valid_cluster_hierarchy(mesh)) return {};
        mesh.recalculateLocalBounds();
        mesh.sourcePath = path;
        return std::make_shared<Mesh>(std::move(mesh));
    }

    bool cook_gltf_mesh(const std::filesystem::path &source) {
        const auto imported = load_gltf_mesh_uncooked(source);
        if (!imported) return false;
        Mesh mesh = *imported;
        auto output = source;
        output.replace_extension(".gmesh");
        for (std::size_t i = 0; i < mesh.images.size(); ++i) {
            auto &image = mesh.images[i];
            if (image.width == 0 || image.height == 0 || image.rgbaPixels.empty()) return false;
            const bool srgb = std::ranges::any_of(mesh.materials, [i](const PBRMaterial &material) {
                const auto index = static_cast<std::int32_t>(i);
                return material.baseColorTexture == index || material.
                       emissiveTexture == index;
            });
            const auto cooked = cook_bc7(image.rgbaPixels, image.width, image.height, srgb);
            const auto texture = texture_path(output, i);
            if (!save_gtex(texture, cooked)) return false;
            image.cookedPath = texture.filename();
            image.cooked.reset();
            image.rgbaPixels.clear();
        }
        return save_gmesh(output, mesh);
    }
} // namespace Engine::Assets
