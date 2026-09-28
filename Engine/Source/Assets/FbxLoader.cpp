#include "FbxLoader.h"
#include "CookCache.h"
#include "DdsImage.h"

#include "Engine/Renderer/Geometry/Mesh.h"
#include "Engine/Renderer/Geometry/Meshlet.h"

#include <stb_image.h>
#include "ThridParty/ufbx.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine::Assets {
namespace {
    using Scene = std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)>;

    std::string utf8_path(const std::filesystem::path& path) {
        const auto value = path.u8string();
        return {reinterpret_cast<const char*>(value.data()), value.size()};
    }

    Scene open_scene(const std::filesystem::path& path) {
        ufbx_load_opts opts{};
        opts.target_axes = ufbx_axes_right_handed_y_up;
        opts.target_unit_meters = 1.0;
        opts.generate_missing_normals = true;
        opts.retain_vertex_attrib_w = true;
        const auto name = utf8_path(path);
        return Scene{ufbx_load_file(name.c_str(), &opts, nullptr), &ufbx_free_scene};
    }

    std::filesystem::path texture_path(const std::filesystem::path& model, const ufbx_texture_file& file) {
        const auto from_string = [](ufbx_string str) -> std::filesystem::path {
            if (!str.data || !str.length) return {};
            std::string value{str.data, str.length};
            std::replace(value.begin(), value.end(), '\\', '/');
            return std::filesystem::path{std::u8string{value.begin(), value.end()}};
        };
        const auto relative = from_string(file.relative_filename);
        if (!relative.empty()) {
            const auto candidate = (model.parent_path() / relative).lexically_normal();
            if (std::filesystem::is_regular_file(candidate)) return candidate;
        }
        const auto resolved = from_string(file.filename);
        if (!resolved.empty() && std::filesystem::is_regular_file(resolved)) return resolved;
        const auto leaf = !relative.empty() ? relative.filename() : resolved.filename();
        if (!leaf.empty()) {
            const auto candidate = model.parent_path() / leaf;
            if (std::filesystem::is_regular_file(candidate)) return candidate;
        }
        return {};
    }

    bool decode_image(std::span<const std::uint8_t> encoded, Mesh::Image& image) {
        if (encoded.empty() || encoded.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
        int width{}, height{}, channels{};
        auto* pixels = stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()),
                                              &width, &height, &channels, STBI_rgb_alpha);
        if (!pixels || width <= 0 || height <= 0) {
            stbi_image_free(pixels);
            return false;
        }
        image.width = static_cast<std::uint32_t>(width);
        image.height = static_cast<std::uint32_t>(height);
        image.rgbaPixels.assign(pixels, pixels + static_cast<std::size_t>(width) * height * STBI_rgb_alpha);
        stbi_image_free(pixels);
        return true;
    }

    std::int32_t image_index(const ufbx_material_map& map, const std::vector<std::int32_t>& files) {
        if (!map.texture || !map.texture_enabled) return -1;
        const ufbx_texture* texture = map.texture;
        if (!texture->has_file && texture->file_textures.count == 1)
            texture = texture->file_textures.data[0];
        if (!texture || !texture->has_file || texture->file_index >= files.size()) return -1;
        return files[texture->file_index];
    }

    glm::vec3 normalize_or(glm::vec3 value, const glm::vec3 fallback) {
        const float length2 = glm::dot(value, value);
        return std::isfinite(length2) && length2 > 1.0e-12F ? value / std::sqrt(length2) : fallback;
    }

    using VertexKey = std::array<std::uint32_t, 16>;
    struct VertexKeyHash {
        std::size_t operator()(const VertexKey& key) const noexcept {
            std::size_t value = 14695981039346656037ULL;
            for (const auto part : key) value = (value ^ part) * 1099511628211ULL;
            return value;
        }
    };

    VertexKey vertex_key(const Vertex& vertex, std::uint32_t logicalIndex) {
        const auto bits = [](float value) { return std::bit_cast<std::uint32_t>(value); };
        const auto n = vertex.normal.native();
        const auto uv = vertex.texCoord.native();
        const auto uv1 = vertex.texCoord1.native();
        const auto color = vertex.color.native();
        const auto tangent = vertex.tangent.native();
        return {logicalIndex, bits(n.x), bits(n.y), bits(n.z), bits(uv.x), bits(uv.y),
                bits(uv1.x), bits(uv1.y), bits(color.x), bits(color.y), bits(color.z),
                bits(tangent.x), bits(tangent.y), bits(tangent.z), bits(tangent.w),
                vertex.materialIndex};
    }

    void accumulate_tangents(const Mesh& mesh, const std::array<std::uint32_t, 3>& indices,
                             std::size_t vertexStart, std::vector<glm::vec3>& tangents,
                             std::vector<glm::vec3>& bitangents) {
        const auto& a = mesh.vertices[indices[0]];
        const auto& b = mesh.vertices[indices[1]];
        const auto& c = mesh.vertices[indices[2]];
        const glm::vec3 e1 = b.position.native() - a.position.native();
        const glm::vec3 e2 = c.position.native() - a.position.native();
        const glm::vec2 uv1 = b.texCoord.native() - a.texCoord.native();
        const glm::vec2 uv2 = c.texCoord.native() - a.texCoord.native();
        const float det = uv1.x * uv2.y - uv1.y * uv2.x;
        const glm::vec3 tangent = std::abs(det) > 1.0e-12F
            ? (e1 * uv2.y - e2 * uv1.y) / det : glm::vec3{};
        const glm::vec3 bitangent = std::abs(det) > 1.0e-12F
            ? (e2 * uv1.x - e1 * uv2.x) / det : glm::vec3{};
        for (const auto index : indices) {
            tangents[index - vertexStart] += tangent;
            bitangents[index - vertexStart] += bitangent;
        }
    }

    void finish_tangents(Mesh& mesh, std::size_t vertexStart,
                         const std::vector<glm::vec3>& tangents,
                         const std::vector<glm::vec3>& bitangents) {
        for (std::size_t i = 0; i < tangents.size(); ++i) {
            auto& vertex = mesh.vertices[vertexStart + i];
            const glm::vec3 normal = normalize_or(vertex.normal.native(), {0, 1, 0});
            const glm::vec3 axis = std::abs(normal.y) < 0.999F ? glm::vec3{0, 1, 0} : glm::vec3{1, 0, 0};
            const glm::vec3 t = normalize_or(tangents[i] - normal * glm::dot(normal, tangents[i]),
                                              normalize_or(glm::cross(axis, normal), {1, 0, 0}));
            const float sign = glm::dot(glm::cross(normal, t), bitangents[i]) < 0 ? -1.0F : 1.0F;
            vertex.tangent = Vec4{t.x, t.y, t.z, sign};
        }
    }

    bool append_node(const ufbx_node& node, const ufbx_scene& scene, Mesh& result) {
        const auto* source = node.mesh;
        if (!source || !source->vertex_position.exists) return true;
        const auto& matrix = node.geometry_to_world;
        const glm::mat3 linear{{static_cast<float>(matrix.m00), static_cast<float>(matrix.m10), static_cast<float>(matrix.m20)},
                               {static_cast<float>(matrix.m01), static_cast<float>(matrix.m11), static_cast<float>(matrix.m21)},
                               {static_cast<float>(matrix.m02), static_cast<float>(matrix.m12), static_cast<float>(matrix.m22)}};
        const float determinant = glm::determinant(linear);
        if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-12F) return false;
        const glm::mat3 normalMatrix = glm::transpose(glm::inverse(linear));
        const bool mirrored = determinant < 0.0F;
        const auto material_for_face = [&](std::size_t face) -> std::uint32_t {
            const auto slot = face < source->face_material.count ? source->face_material.data[face] : 0;
            if (slot >= node.materials.count || !node.materials.data[slot])
                return static_cast<std::uint32_t>(scene.materials.count);
            return node.materials.data[slot]->typed_id;
        };
        const bool hasTangents = source->vertex_tangent.exists;
        const std::size_t vertexStart = result.vertices.size();
        std::unordered_map<VertexKey, std::uint32_t, VertexKeyHash> vertexLookup;
        vertexLookup.reserve(std::min<std::size_t>(source->num_indices, 1'000'000));
        std::vector<glm::vec3> tangentSums, bitangentSums;
        std::uint32_t currentMaterial = std::numeric_limits<std::uint32_t>::max();
        for (std::size_t faceIndex = 0; faceIndex < source->faces.count; ++faceIndex) {
            if (source->face_hole.count > faceIndex && source->face_hole.data[faceIndex]) continue;
            const auto face = source->faces.data[faceIndex];
            if (face.num_indices < 3) continue;
            const auto material = material_for_face(faceIndex);
            if (material != currentMaterial) {
                currentMaterial = material;
                result.renderSections.push_back({.firstIndex = static_cast<std::uint32_t>(result.indices.size()),
                                                 .materialIndex = material});
            }
            std::vector<std::uint32_t> corners((static_cast<std::size_t>(face.num_indices) - 2) * 3);
            const auto triangleCount = ufbx_triangulate_face(corners.data(), corners.size(), source, face);
            for (std::uint32_t triangle = 0; triangle < triangleCount; ++triangle) {
                std::array<std::uint32_t, 3> triangleIndices{};
                for (int corner = 0; corner < 3; ++corner) {
                    const auto sourceCorner = corners[triangle * 3 + (mirrored ? 2 - corner : corner)];
                    if (sourceCorner >= source->num_indices) return false;
                    const auto position = ufbx_get_vertex_vec3(&source->vertex_position, sourceCorner);
                    const glm::vec3 world = linear * glm::vec3{position.x, position.y, position.z} +
                        glm::vec3{matrix.m03, matrix.m13, matrix.m23};
                    Vertex vertex{};
                    vertex.position = Vec3{world};
                    vertex.color = Vec3{1, 1, 1};
                    vertex.materialIndex = material;
                    if (source->vertex_normal.exists) {
                        const auto n = ufbx_get_vertex_vec3(&source->vertex_normal, sourceCorner);
                        vertex.normal = Vec3{normalize_or(normalMatrix * glm::vec3{n.x, n.y, n.z}, {0, 1, 0})};
                    }
                    if (source->vertex_uv.exists) {
                        const auto uv = ufbx_get_vertex_vec2(&source->vertex_uv, sourceCorner);
                        vertex.texCoord = Vec2{static_cast<float>(uv.x), static_cast<float>(1.0 - uv.y)};
                    }
                    vertex.texCoord1 = vertex.texCoord;
                    if (source->uv_sets.count > 1 && source->uv_sets.data[1].vertex_uv.exists) {
                        const auto uv = ufbx_get_vertex_vec2(&source->uv_sets.data[1].vertex_uv, sourceCorner);
                        vertex.texCoord1 = Vec2{static_cast<float>(uv.x), static_cast<float>(1.0 - uv.y)};
                    }
                    if (source->vertex_color.exists) {
                        const auto color = ufbx_get_vertex_vec4(&source->vertex_color, sourceCorner);
                        vertex.color = Vec3{static_cast<float>(color.x), static_cast<float>(color.y),
                                            static_cast<float>(color.z)};
                    }
                    if (hasTangents) {
                        const auto tangent = ufbx_get_vertex_vec3(&source->vertex_tangent, sourceCorner);
                        const auto t = normalize_or(linear * glm::vec3{tangent.x, tangent.y, tangent.z}, {1, 0, 0});
                        float sign = 1.0F;
                        if (source->vertex_bitangent.exists) {
                            const auto bitangent = ufbx_get_vertex_vec3(&source->vertex_bitangent, sourceCorner);
                            sign = glm::dot(glm::cross(vertex.normal.native(), t),
                                            linear * glm::vec3{bitangent.x, bitangent.y, bitangent.z}) < 0 ? -1.0F : 1.0F;
                        }
                        // UV V is inverted above to match the engine texture convention.
                        vertex.tangent = Vec4{t.x, t.y, t.z, -sign};
                    }
                    const auto key = vertex_key(vertex, source->vertex_indices.data[sourceCorner]);
                    if (const auto found = vertexLookup.find(key); found != vertexLookup.end()) {
                        triangleIndices[corner] = found->second;
                        result.indices.push_back(found->second);
                        continue;
                    }
                    if (result.vertices.size() >= std::numeric_limits<std::uint32_t>::max()) return false;
                    const auto index = static_cast<std::uint32_t>(result.vertices.size());
                    vertexLookup.emplace(key, index);
                    triangleIndices[corner] = index;
                    result.indices.push_back(index);
                    result.vertices.push_back(vertex);
                    if (!hasTangents) {
                        tangentSums.emplace_back(0.0F);
                        bitangentSums.emplace_back(0.0F);
                    }
                }
                if (!hasTangents) accumulate_tangents(result, triangleIndices, vertexStart,
                                                      tangentSums, bitangentSums);
                result.renderSections.back().indexCount += 3;
            }
        }
        if (!hasTangents) finish_tangents(result, vertexStart, tangentSums, bitangentSums);
        return true;
    }
}

std::shared_ptr<const Mesh> load_fbx_mesh(const std::filesystem::path& path, bool forCooking) {
    const auto scene = open_scene(path);
    if (!scene || scene->materials.count >= std::numeric_limits<std::uint32_t>::max()) return {};
    Mesh result;
    std::vector<std::int32_t> imageIndices(scene->texture_files.count, -1);
    for (std::size_t i = 0; i < scene->texture_files.count; ++i) {
        const auto& file = scene->texture_files.data[i];
        Mesh::Image image;
        if (file.content.size) image.sourceImageIndex = static_cast<std::uint32_t>(i);
        else image.sourcePath = texture_path(path, file);
        if (image.sourcePath.empty() && !file.content.size) continue;
        if (!forCooking) {
            if (file.content.size) {
                if (!decode_image({static_cast<const std::uint8_t*>(file.content.data), file.content.size}, image)) return {};
            } else if (image.sourcePath.extension() == ".dds" || image.sourcePath.extension() == ".DDS") {
                if (!decode_dds_image(image.sourcePath, image.rgbaPixels, image.width, image.height)) return {};
            } else {
                int w{}, h{}, channels{};
                const auto filename = utf8_path(image.sourcePath);
                auto* pixels = stbi_load(filename.c_str(), &w, &h, &channels, STBI_rgb_alpha);
                if (!pixels) return {};
                image.width = static_cast<std::uint32_t>(w);
                image.height = static_cast<std::uint32_t>(h);
                image.rgbaPixels.assign(pixels, pixels + static_cast<std::size_t>(w) * h * STBI_rgb_alpha);
                stbi_image_free(pixels);
            }
        }
        imageIndices[i] = static_cast<std::int32_t>(result.images.size());
        result.images.push_back(std::move(image));
    }
    for (std::size_t i = 0; i < scene->materials.count; ++i) {
        const auto& source = *scene->materials.data[i];
        PBRMaterial material;
        const auto& base = source.pbr.base_color.has_value ? source.pbr.base_color : source.fbx.diffuse_color;
        const float opacity = source.pbr.opacity.has_value
            ? static_cast<float>(source.pbr.opacity.value_real)
            : source.fbx.transparency_factor.has_value
                ? 1.0F - static_cast<float>(source.fbx.transparency_factor.value_real) : 1.0F;
        if (base.has_value)
            material.baseColor = Math::Color{static_cast<float>(base.value_vec3.x),
                                             static_cast<float>(base.value_vec3.y),
                                             static_cast<float>(base.value_vec3.z), opacity};
        if (source.pbr.metalness.has_value) material.metallic = static_cast<float>(source.pbr.metalness.value_real);
        if (source.pbr.roughness.has_value) material.roughness = static_cast<float>(source.pbr.roughness.value_real);
        material.baseColorTexture = image_index(source.pbr.base_color, imageIndices);
        if (material.baseColorTexture < 0) material.baseColorTexture = image_index(source.fbx.diffuse_color, imageIndices);
        material.normalTexture = image_index(source.pbr.normal_map, imageIndices);
        if (material.normalTexture < 0) material.normalTexture = image_index(source.fbx.normal_map, imageIndices);
        material.aoTexture = image_index(source.pbr.ambient_occlusion, imageIndices);
        material.emissiveTexture = image_index(source.pbr.emission_color, imageIndices);
        material.opacityTexture = image_index(source.pbr.opacity, imageIndices);
        material.displacementTexture = image_index(source.pbr.displacement_map, imageIndices);
        material.specularColorTexture = image_index(source.pbr.specular_color, imageIndices);
        if (opacity < 1.0F) material.alphaMode = AlphaMode::Blend;
        if (material.opacityTexture >= 0) material.alphaMode = AlphaMode::Mask;
        material.doubleSided = source.features.double_sided.enabled;
        result.materials.push_back(material);
    }
    result.materials.emplace_back();
    for (std::size_t i = 0; i < scene->nodes.count; ++i) {
        if (!append_node(*scene->nodes.data[i], *scene, result)) return {};
    }
    if (result.empty()) return {};
    for (auto& section : result.renderSections) {
        if (!section.indexCount) continue;
        glm::vec3 low{std::numeric_limits<float>::max()}, high{std::numeric_limits<float>::lowest()};
        for (std::size_t i = section.firstIndex; i < section.firstIndex + section.indexCount; ++i) {
            const auto p = result.vertices[result.indices[i]].position.native();
            low = glm::min(low, p);
            high = glm::max(high, p);
        }
        section.localBounds = {Vec3{low}, Vec3{high}};
    }
    result.renderSections.erase(std::remove_if(result.renderSections.begin(), result.renderSections.end(),
                                               [](const auto& section) { return section.indexCount == 0; }),
                                result.renderSections.end());
    result.recalculateLocalBounds();
    result.sourcePath = path;
    if (!forCooking && !build_meshlets(result)) return {};
    return std::make_shared<const Mesh>(std::move(result));
}

std::optional<std::uint64_t> fbx_source_hash(const std::filesystem::path& path) {
    const auto sourceHash = CookCache::hash_file(path);
    if (!sourceHash) return std::nullopt;
    const auto scene = open_scene(path);
    if (!scene) return std::nullopt;
    CookCache::Hash64 hash;
    hash.add("fbx-dependencies-v1");
    hash.add(*sourceHash);
    for (std::size_t i = 0; i < scene->texture_files.count; ++i) {
        const auto& file = scene->texture_files.data[i];
        if (file.content.size) continue;
        const auto dependency = texture_path(path, file);
        if (dependency.empty()) continue;
        const auto contentHash = CookCache::hash_file(dependency);
        if (!contentHash) return std::nullopt;
        hash.add(dependency.lexically_normal().generic_string());
        hash.add(*contentHash);
    }
    return hash.value;
}

bool visit_fbx_embedded_images(const std::filesystem::path& path, const FbxEmbeddedImageVisitor& visitor) {
    const auto scene = open_scene(path);
    if (!scene) return false;
    for (std::size_t i = 0; i < scene->texture_files.count; ++i) {
        const auto content = scene->texture_files.data[i].content;
        if (!content.size) continue;
        Mesh::Image image;
        if (!decode_image({static_cast<const std::uint8_t*>(content.data), content.size}, image) ||
            !visitor(static_cast<std::uint32_t>(i), image.rgbaPixels, image.width, image.height)) return false;
    }
    return true;
}
}
