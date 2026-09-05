#include "Engine/Assets/AssetManager.h"

#include "GlbLoader.h"
#include "Engine/Renderer/Geometry/Mesh.h"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>

// NOLINTBEGIN(readability-magic-numbers)

namespace Engine::Assets {
    namespace {
        struct ObjIndex {
            int position{};
            int tex_coord{};
            int normal{};

            bool operator==(const ObjIndex &) const = default;
        };

        struct ObjIndexHash {
            std::size_t operator()(const ObjIndex &index) const noexcept {
                const auto combine = [](std::size_t seed, int value) {
                    return seed ^ (std::hash<int>{}(value) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U));
                };
                return combine(combine(std::hash<int>{}(index.position), index.tex_coord), index.normal);
            }
        };

        int resolve_obj_index(const int index, const std::size_t count) {
            if (index > 0) { return index - 1;
}
            if (index < 0) { return static_cast<int>(count) + index;
}
            return -1;
        }

        bool parse_obj_index(std::string_view token, ObjIndex &result) {
            result = {};
            std::size_t start = 0;
            int *fields[] = {&result.position, &result.tex_coord, &result.normal};
            for (int field = 0; field < 3 && start <= token.size(); ++field) {
                const auto end = token.find('/', start);
                const auto part = token.substr(
                    start, end == std::string_view::npos ? token.size() - start : end - start);
                if (!part.empty()) {
                    try {
                        *fields[field] = std::stoi(std::string(part));
                    } catch (...) {
                        return false;
                    }
                }
                if (end == std::string_view::npos) { break;
}
                start = end + 1;
            }
            return result.position != 0;
        }

        std::shared_ptr<const Mesh> load_obj_mesh(const std::filesystem::path &path) {
            std::ifstream file(path);
            if (!file) { return {};
}

            std::vector<Vec3> positions;
            std::vector<Vec2> tex_coords;
            std::vector<Vec3> normals;
            Mesh mesh;
            std::unordered_map<ObjIndex, std::uint32_t, ObjIndexHash> vertices;
            bool has_normals = true;
            std::string line;

            const auto add_vertex = [&](const ObjIndex &source) -> std::optional<std::uint32_t> {
                const ObjIndex resolved{
                    resolve_obj_index(source.position, positions.size()),
                    source.tex_coord == 0 ? -1 : resolve_obj_index(source.tex_coord, tex_coords.size()),
                    source.normal == 0 ? -1 : resolve_obj_index(source.normal, normals.size())
                };
                if (resolved.position < 0 || static_cast<std::size_t>(resolved.position) >= positions.size() ||
                    (resolved.tex_coord >= 0 && static_cast<std::size_t>(resolved.tex_coord) >= tex_coords.size()) ||
                    (resolved.normal >= 0 && static_cast<std::size_t>(resolved.normal) >= normals.size())) {
                    return std::nullopt;
}

                if (const auto found = vertices.find(resolved); found != vertices.end()) { return found->second;
}
                Vertex vertex;
                vertex.position = positions[resolved.position];
                vertex.color = Vec3{1.0F, 1.0F, 1.0F};
                if (resolved.tex_coord >= 0) { vertex.texCoord = tex_coords[resolved.tex_coord];
}
                if (resolved.normal >= 0) { vertex.normal = normals[resolved.normal];
                } else { has_normals = false;
}
                const auto index = static_cast<std::uint32_t>(mesh.vertices.size());
                mesh.vertices.push_back(vertex);
                vertices.emplace(resolved, index);
                return index;
            };

            while (std::getline(file, line)) {
                std::istringstream stream(line);
                std::string command;
                stream >> command;
                if (command.empty() || command[0] == '#') { continue;
}
                if (command == "v") {
                    float x;
                    float y;
                    float z;
                    if (!(stream >> x >> y >> z)) { return {};
}
                    positions.emplace_back(x, y, z);
                } else if (command == "vt") {
                    float u;
                    float v;
                    if (!(stream >> u >> v)) { return {};
}
                    tex_coords.emplace_back(u, v);
                } else if (command == "vn") {
                    float x;
                    float y;
                    float z;
                    if (!(stream >> x >> y >> z)) { return {};
}
                    normals.emplace_back(x, y, z);
                } else if (command == "f") {
                    std::vector<std::uint32_t> face;
                    std::string token;
                    while (stream >> token) {
                        ObjIndex source;
                        if (!parse_obj_index(token, source)) { return {};
}
                        const auto index = add_vertex(source);
                        if (!index) { return {};
}
                        face.push_back(*index);
                    }
                    if (face.size() < 3) { return {};
}
                    for (std::size_t i = 1; i + 1 < face.size(); ++i) {
                        mesh.indices.insert(mesh.indices.end(), {face[0], face[i], face[i + 1]});
                    }
                }
            }

            if (mesh.empty()) { return {};
}
            mesh.sourcePath = path;
            if (!has_normals) {
                for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                    auto &a = mesh.vertices[mesh.indices[i]];
                    auto &b = mesh.vertices[mesh.indices[i + 1]];
                    auto &c = mesh.vertices[mesh.indices[i + 2]];
                    const auto normal = cross(b.position - a.position, c.position - a.position).normalized();
                    a.normal += normal;
                    b.normal += normal;
                    c.normal += normal;
                }
                for (auto &vertex: mesh.vertices) {
                    if (vertex.normal.length() > 0.0F) { vertex.normal = vertex.normal.normalized();
                    } else { vertex.normal = Vec3{0.0F, 1.0F, 0.0F};
}
                }
            }
            return std::make_shared<const Mesh>(std::move(mesh));
        }
    } // namespace

    AssetManager::AssetManager(std::filesystem::path asset_root) : asset_root_(std::move(asset_root)) {
        register_default_asset_loaders(*this);
    }

    void AssetManager::set_asset_root(std::filesystem::path root) {
        std::scoped_lock lock(mutex_);
        asset_root_ = std::move(root);
    }

    void AssetManager::set_error_handler(ErrorHandler handler) {
        std::scoped_lock lock(mutex_);
        error_handler_ = std::move(handler);
    }

    std::filesystem::path AssetManager::resolve(const std::filesystem::path &path) const {
        if (path.is_absolute() || asset_root_.empty()) { return path.lexically_normal();
}
        return (asset_root_ / path).lexically_normal();
    }

    std::string AssetManager::normalize_path(const std::filesystem::path &path) {
        auto result = path.lexically_normal().generic_string();
        std::ranges::transform(result, result.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return result;
    }

    std::string AssetManager::make_key(const std::filesystem::path &path) const {
        return normalize_path(resolve(path));
    }

    AssetId AssetManager::make_id(std::string_view value) noexcept {
        // FNV-1a is stable between runs and platforms, unlike std::hash.
        std::uint64_t hash = 14695981039346656037ULL;
        for (const auto c: value) {
            hash ^= static_cast<std::uint8_t>(c);
            hash *= 1099511628211ULL;
        }
        return hash == 0 ? 1 : hash;
    }

    std::size_t AssetManager::CacheKeyHash::operator()(const CacheKey &key) const noexcept {
        const auto a = std::hash<AssetId>{}(key.id);
        const auto b = key.type.hash_code();
        const auto c = std::hash<std::string>{}(key.path);
        return (a ^ (b + 0x9e3779b9U + (a << 6U) + (a >> 2U))) ^
               (c + 0x9e3779b9U + (a << 6U) + (a >> 2U));
    }

    void AssetManager::report(const std::string &message) const {
        ErrorHandler handler;
        {
            std::scoped_lock lock(mutex_);
            handler = error_handler_;
        }
        if (handler) { handler(message);
}
    }

    void AssetManager::unload_unused() {
        std::scoped_lock lock(mutex_);
        for (auto it = cache_.begin(); it != cache_.end();) {
            if (it->second.value.use_count() == 1) { it = cache_.erase(it);
            } else { ++it;
}
        }
    }

    void AssetManager::clear() {
        std::scoped_lock lock(mutex_);
        cache_.clear();
    }

    bool AssetManager::contains(AssetId id, std::type_index type) const {
        std::scoped_lock lock(mutex_);
        return std::ranges::any_of(cache_, [id, type](const auto &entry) {
            return entry.first.id == id && entry.first.type == type;
        });
    }

    std::size_t AssetManager::size() const {
        std::scoped_lock lock(mutex_);
        return cache_.size();
    }

    void register_default_asset_loaders(AssetManager &manager) {
        const auto text_loader = [](const std::filesystem::path &path, const AssetMetadata &) {
            std::ifstream file(path, std::ios::binary);
            if (!file) { return std::shared_ptr<const TextAsset>{};
}
            std::ostringstream stream;
            stream << file.rdbuf();
            return std::make_shared<const TextAsset>(TextAsset{stream.str()});
        };

        const auto binary_loader = [](const std::filesystem::path &path, const AssetMetadata &) {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file) { return std::shared_ptr<const BinaryAsset>{};
}
            const auto size = file.tellg();
            if (size < 0) { return std::shared_ptr<const BinaryAsset>{};
}
            BinaryAsset asset;
            asset.bytes.resize(static_cast<std::size_t>(size));
            file.seekg(0);
            file.read(reinterpret_cast<char *>(asset.bytes.data()), size);
            return std::make_shared<const BinaryAsset>(std::move(asset));
        };

        manager.register_loader<TextAsset>(AssetType::Text, text_loader);
        manager.register_loader<ShaderAsset>(AssetType::Shader, [text_loader](const auto &path, const auto &metadata) {
            auto text = text_loader(path, metadata);
            if (!text) { return std::shared_ptr<const ShaderAsset>{};
}
            return std::make_shared<const ShaderAsset>(ShaderAsset{text->text, "main"});
        });
        manager.register_loader<BinaryAsset>(AssetType::Binary, binary_loader);
        manager.register_loader<TextureAsset>(AssetType::Texture2D, [](const auto &path, const auto &) {
            int width{};
            int height{};
            int channels{};
            stbi_uc *pixels = stbi_load(path.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
            if (!pixels || width <= 0 || height <= 0) {
                stbi_image_free(pixels);
                return std::shared_ptr<const TextureAsset>{};
            }
            TextureAsset texture;
            texture.width = static_cast<std::uint32_t>(width);
            texture.height = static_cast<std::uint32_t>(height);
            texture.rgbaPixels.assign(pixels, pixels + (static_cast<std::size_t>(width) * height * STBI_rgb_alpha));
            stbi_image_free(pixels);
            return std::make_shared<const TextureAsset>(std::move(texture));
        });
        manager.register_loader<PBRMaterial>(AssetType::Material, [](const auto &path, const auto &) {
            std::ifstream file(path);
            if (!file) { return std::shared_ptr<const PBRMaterial>{};
}
            PBRMaterial material{};
            float red{};
            float green{};
            float blue{};
            float alpha{};
            int alphaMode{};
            if (!(file >> red >> green >> blue >> alpha
                  >> material.metallic >> material.roughness >> material.aoStrength
                  >> material.baseColorTexture >> material.metallicRoughnessTexture
                  >> material.normalTexture >> material.normalScale
                  >> alphaMode >> material.doubleSided >> material.alphaCutoff)) {
                return std::shared_ptr<const PBRMaterial>{};
            }
            if (alphaMode < 0 || alphaMode > static_cast<int>(AlphaMode::Blend))
                return std::shared_ptr<const PBRMaterial>{};
            material.alphaMode = static_cast<AlphaMode>(alphaMode);
            material.baseColor = Math::Color{red, green, blue, alpha};
            return std::make_shared<const PBRMaterial>(material);
        });
        manager.register_loader<Mesh>(AssetType::Mesh, [](const auto &path, const auto &) {
            const auto extension = path.extension().string();
            if (extension == ".obj" || extension == ".OBJ") { return load_obj_mesh(path);
}
            if (extension == ".glb" || extension == ".GLB" ||
                extension == ".gltf" || extension == ".GLTF") {
                return load_gltf_mesh(path);
}
            return std::shared_ptr<const Mesh>{};
        });
    }
}

// NOLINTEND(readability-magic-numbers)
