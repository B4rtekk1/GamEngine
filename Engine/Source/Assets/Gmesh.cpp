#include "Engine/Assets/Gmesh.h"

#include "Engine/Assets/Gtex.h"
#include "GlbLoader.h"
#include "Engine/Assets/TextureCooker.h"
#include "Engine/Renderer/Geometry/Mesh.h"

#include <array>
#include <fstream>
#include <limits>
#include <ranges>
#include <string>
#include <type_traits>

namespace Engine::Assets {
namespace {
constexpr std::array<char, 8> magic{'G', 'M', 'E', 'S', 'H', '\0', '\0', '\0'};
constexpr std::uint32_t version = 1;
constexpr std::uint32_t maxElements = 100'000'000;
struct Header { std::array<char, 8> magic; std::uint32_t version; std::uint32_t vertices; std::uint32_t indices; std::uint32_t materials; std::uint32_t images; };
static_assert(std::is_trivially_copyable_v<Vertex>);
static_assert(std::is_trivially_copyable_v<PBRMaterial>);
template<class T> bool write(std::ofstream& file, const T& value) { file.write(reinterpret_cast<const char*>(&value), sizeof value); return static_cast<bool>(file); }
template<class T> bool read(std::ifstream& file, T& value) { file.read(reinterpret_cast<char*>(&value), sizeof value); return static_cast<bool>(file); }
template<class T> bool write_vector(std::ofstream& file, const std::vector<T>& values) { if (values.empty()) return true; file.write(reinterpret_cast<const char*>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(T))); return static_cast<bool>(file); }
template<class T> bool read_vector(std::ifstream& file, std::vector<T>& values) { if (values.empty()) return true; file.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(T))); return static_cast<bool>(file); }
std::filesystem::path texture_path(const std::filesystem::path& mesh, const std::size_t index) { return mesh.parent_path() / (mesh.stem().string() + ".image" + std::to_string(index) + ".gtex"); }
}

bool save_gmesh(const std::filesystem::path& path, const Mesh& mesh) {
    if (mesh.vertices.size() > maxElements || mesh.indices.size() > maxElements || mesh.materials.size() > maxElements || mesh.images.size() > maxElements) return false;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    const Header header{magic, version, static_cast<std::uint32_t>(mesh.vertices.size()), static_cast<std::uint32_t>(mesh.indices.size()), static_cast<std::uint32_t>(mesh.materials.size()), static_cast<std::uint32_t>(mesh.images.size())};
    if (!file || !write(file, header) || !write_vector(file, mesh.vertices) || !write_vector(file, mesh.indices) || !write_vector(file, mesh.materials)) return false;
    for (const auto& image : mesh.images) { const auto pathText = image.cookedPath.generic_string(); const auto length = static_cast<std::uint32_t>(pathText.size()); if (!write(file, length)) return false; file.write(pathText.data(), length); if (!file) return false; }
    return true;
}

std::shared_ptr<const Mesh> load_gmesh(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary); Header header{};
    if (!file || !read(file, header) || header.magic != magic || header.version != version || header.vertices > maxElements || header.indices > maxElements || header.materials > maxElements || header.images > maxElements) return {};
    Mesh mesh; mesh.vertices.resize(header.vertices); mesh.indices.resize(header.indices); mesh.materials.resize(header.materials); mesh.images.resize(header.images);
    if (!read_vector(file, mesh.vertices) || !read_vector(file, mesh.indices) || !read_vector(file, mesh.materials)) return {};
    for (auto& image : mesh.images) { std::uint32_t length{}; if (!read(file, length) || length > 32'768) return {}; std::string text(length, '\0'); file.read(text.data(), length); if (!file) return {}; image.cookedPath = text; auto gtex = load_gtex(path.parent_path() / image.cookedPath); if (!gtex) return {}; image.width = gtex->width; image.height = gtex->height; image.gtex.emplace(std::move(*gtex)); }
    if (mesh.empty()) return {}; mesh.sourcePath = path; return std::make_shared<const Mesh>(std::move(mesh));
}

bool cook_gltf_mesh(const std::filesystem::path& source) {
    const auto imported = load_gltf_mesh_uncooked(source); if (!imported) return false;
    Mesh mesh = *imported; auto output = source; output.replace_extension(".gmesh");
    for (std::size_t i = 0; i < mesh.images.size(); ++i) { auto& image = mesh.images[i]; if (image.width == 0 || image.height == 0 || image.rgbaPixels.empty()) return false; const bool srgb = std::ranges::any_of(mesh.materials, [i](const PBRMaterial& material) { const auto index = static_cast<std::int32_t>(i); return material.baseColorTexture == index || material.emissiveTexture == index; }); const auto cooked = cook_bc7(image.rgbaPixels, image.width, image.height, srgb); const auto texture = texture_path(output, i); if (!save_gtex(texture, cooked)) return false; image.cookedPath = texture.filename(); image.cooked.reset(); image.rgbaPixels.clear(); }
    return save_gmesh(output, mesh);
}
} // namespace Engine::Assets
