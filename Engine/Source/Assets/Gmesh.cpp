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
constexpr std::uint32_t version = 2;
constexpr std::uint32_t maxElements = 100'000'000;
struct HeaderV1 { std::array<char, 8> magic; std::uint32_t version; std::uint32_t vertices; std::uint32_t indices; std::uint32_t materials; std::uint32_t images; };
struct FilePrefix final { std::array<char, 8> magic; std::uint32_t version; };
struct Header final { std::array<char, 8> magic; std::uint32_t version; std::uint32_t vertices; std::uint32_t indices; std::uint32_t meshlets; std::uint32_t meshletVertices; std::uint32_t meshletTriangles; std::uint32_t materials; std::uint32_t images; };
static_assert(std::is_trivially_copyable_v<Vertex>);
static_assert(std::is_trivially_copyable_v<PBRMaterial>);
static_assert(std::is_trivially_copyable_v<Meshlet>);
template<class T> bool write(std::ofstream& file, const T& value) { file.write(reinterpret_cast<const char*>(&value), sizeof value); return static_cast<bool>(file); }
template<class T> bool read(std::ifstream& file, T& value) { file.read(reinterpret_cast<char*>(&value), sizeof value); return static_cast<bool>(file); }
template<class T> bool write_vector(std::ofstream& file, const std::vector<T>& values) { if (values.empty()) return true; file.write(reinterpret_cast<const char*>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(T))); return static_cast<bool>(file); }
template<class T> bool read_vector(std::ifstream& file, std::vector<T>& values) { if (values.empty()) return true; file.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(T))); return static_cast<bool>(file); }
std::filesystem::path texture_path(const std::filesystem::path& mesh, const std::size_t index) { return mesh.parent_path() / (mesh.stem().string() + ".image" + std::to_string(index) + ".gtex"); }
[[nodiscard]] bool valid_meshlets(const Mesh& mesh) {
    for (const Meshlet& meshlet : mesh.meshlets) {
        if (meshlet.vertexCount == 0 || meshlet.vertexCount > MeshletBuildOptions::MaxVertices ||
            meshlet.triangleCount == 0 || meshlet.triangleCount > MeshletBuildOptions::MaxTriangles ||
            meshlet.vertexOffset > mesh.meshletVertices.size() ||
            meshlet.vertexCount > mesh.meshletVertices.size() - meshlet.vertexOffset ||
            meshlet.triangleOffset > mesh.meshletTriangles.size() ||
            meshlet.triangleCount > mesh.meshletTriangles.size() - meshlet.triangleOffset) return false;
        for (std::uint32_t index = 0; index < meshlet.vertexCount; ++index)
            if (mesh.meshletVertices[meshlet.vertexOffset + index] >= mesh.vertices.size()) return false;
        for (std::uint32_t index = 0; index < meshlet.triangleCount; ++index) {
            const std::uint32_t triangle = mesh.meshletTriangles[meshlet.triangleOffset + index];
            if ((triangle & 0xffU) >= meshlet.vertexCount || ((triangle >> 8U) & 0xffU) >= meshlet.vertexCount ||
                ((triangle >> 16U) & 0xffU) >= meshlet.vertexCount || (triangle >> 24U) != 0U) return false;
        }
    }
    return !mesh.indices.empty() ? !mesh.meshlets.empty() : mesh.meshlets.empty();
}
}

bool save_gmesh(const std::filesystem::path& path, const Mesh& mesh) {
    Mesh cooked = mesh;
    if (!build_meshlets(cooked)) return false;
    if (cooked.vertices.size() > maxElements || cooked.indices.size() > maxElements || cooked.meshlets.size() > maxElements || cooked.meshletVertices.size() > maxElements || cooked.meshletTriangles.size() > maxElements || cooked.materials.size() > maxElements || cooked.images.size() > maxElements) return false;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    const Header header{magic, version, static_cast<std::uint32_t>(cooked.vertices.size()), static_cast<std::uint32_t>(cooked.indices.size()), static_cast<std::uint32_t>(cooked.meshlets.size()), static_cast<std::uint32_t>(cooked.meshletVertices.size()), static_cast<std::uint32_t>(cooked.meshletTriangles.size()), static_cast<std::uint32_t>(cooked.materials.size()), static_cast<std::uint32_t>(cooked.images.size())};
    if (!file || !write(file, header) || !write_vector(file, cooked.vertices) || !write_vector(file, cooked.indices) || !write_vector(file, cooked.meshlets) || !write_vector(file, cooked.meshletVertices) || !write_vector(file, cooked.meshletTriangles) || !write_vector(file, cooked.materials)) return false;
    for (const auto& image : cooked.images) { const auto pathText = image.cookedPath.generic_string(); const auto length = static_cast<std::uint32_t>(pathText.size()); if (!write(file, length)) return false; file.write(pathText.data(), length); if (!file) return false; }
    return true;
}

std::shared_ptr<const Mesh> load_gmesh(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary); FilePrefix prefix{};
    if (!file || !read(file, prefix) || prefix.magic != magic || (prefix.version != 1 && prefix.version != version)) return {};
    file.seekg(0);
    Mesh mesh;
    if (prefix.version == version) {
        Header header{};
        if (!read(file, header) || header.vertices > maxElements || header.indices > maxElements || header.meshlets > maxElements || header.meshletVertices > maxElements || header.meshletTriangles > maxElements || header.materials > maxElements || header.images > maxElements) return {};
        mesh.vertices.resize(header.vertices); mesh.indices.resize(header.indices); mesh.meshlets.resize(header.meshlets); mesh.meshletVertices.resize(header.meshletVertices); mesh.meshletTriangles.resize(header.meshletTriangles); mesh.materials.resize(header.materials); mesh.images.resize(header.images);
        if (!read_vector(file, mesh.vertices) || !read_vector(file, mesh.indices) || !read_vector(file, mesh.meshlets) || !read_vector(file, mesh.meshletVertices) || !read_vector(file, mesh.meshletTriangles) || !read_vector(file, mesh.materials)) return {};
    } else {
        HeaderV1 header{};
        if (!read(file, header) || header.vertices > maxElements || header.indices > maxElements || header.materials > maxElements || header.images > maxElements) return {};
        mesh.vertices.resize(header.vertices); mesh.indices.resize(header.indices); mesh.materials.resize(header.materials); mesh.images.resize(header.images);
        if (!read_vector(file, mesh.vertices) || !read_vector(file, mesh.indices) || !read_vector(file, mesh.materials) || !build_meshlets(mesh)) return {};
    }
    for (auto& image : mesh.images) { std::uint32_t length{}; if (!read(file, length) || length > 32'768) return {}; std::string text(length, '\0'); file.read(text.data(), length); if (!file) return {}; image.cookedPath = text; auto gtex = load_gtex(path.parent_path() / image.cookedPath); if (!gtex) return {}; image.width = gtex->width; image.height = gtex->height; image.gtex.emplace(std::move(*gtex)); }
    if (mesh.empty() || !valid_meshlets(mesh)) return {}; mesh.sourcePath = path; return std::make_shared<const Mesh>(std::move(mesh));
}

bool cook_gltf_mesh(const std::filesystem::path& source) {
    const auto imported = load_gltf_mesh_uncooked(source); if (!imported) return false;
    Mesh mesh = *imported; auto output = source; output.replace_extension(".gmesh");
    for (std::size_t i = 0; i < mesh.images.size(); ++i) { auto& image = mesh.images[i]; if (image.width == 0 || image.height == 0 || image.rgbaPixels.empty()) return false; const bool srgb = std::ranges::any_of(mesh.materials, [i](const PBRMaterial& material) { const auto index = static_cast<std::int32_t>(i); return material.baseColorTexture == index || material.emissiveTexture == index; }); const auto cooked = cook_bc7(image.rgbaPixels, image.width, image.height, srgb); const auto texture = texture_path(output, i); if (!save_gtex(texture, cooked)) return false; image.cookedPath = texture.filename(); image.cooked.reset(); image.rgbaPixels.clear(); }
    return save_gmesh(output, mesh);
}
} // namespace Engine::Assets
