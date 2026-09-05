#include "GlbLoader.h"

#include "Engine/Renderer/Geometry/Mesh.h"

#include <stb_image.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
#define CGLTF_IMPLEMENTATION
#include "ThridParty/cgltf.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <glm/gtc/type_ptr.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

// NOLINTBEGIN(readability-magic-numbers)

namespace Engine::Assets {
namespace {

using GltfData = std::unique_ptr<cgltf_data, decltype(&cgltf_free)>;

[[nodiscard]] const cgltf_accessor* attribute(const cgltf_primitive& primitive,
                                               const cgltf_attribute_type type,
                                               const cgltf_int index = 0) {
    for (cgltf_size i = 0; i < primitive.attributes_count; ++i) {
        const cgltf_attribute& candidate = primitive.attributes[i];
        if (candidate.type == type && candidate.index == index) return candidate.data;
    }
    return nullptr;
}

[[nodiscard]] std::int32_t image_index(const cgltf_data& data,
                                       const cgltf_texture_view& view) {
    if (view.texture == nullptr || view.texture->image == nullptr) return -1;
    const auto index = view.texture->image - data.images;
    if (index < 0 || static_cast<cgltf_size>(index) >= data.images_count) return -1;
    return static_cast<std::int32_t>(index);
}

[[nodiscard]] std::vector<std::uint8_t> decode_base64(std::string_view source) {
    static constexpr std::array<std::int8_t, 256> table = [] {
        std::array<std::int8_t, 256> result{};
        result.fill(-1);
        for (std::int8_t i = 0; i < 26; ++i) {
            result[static_cast<unsigned char>('A' + i)] = i;
            result[static_cast<unsigned char>('a' + i)] = static_cast<std::int8_t>(26 + i);
        }
        for (std::int8_t i = 0; i < 10; ++i) {
            result[static_cast<unsigned char>('0' + i)] = static_cast<std::int8_t>(52 + i);
        }
        result[static_cast<unsigned char>('+')] = 62;
        result[static_cast<unsigned char>('/')] = 63;
        return result;
    }();

    std::vector<std::uint8_t> bytes;
    bytes.reserve((source.size() * 3) / 4);
    std::uint32_t accumulator = 0;
    unsigned bits = 0;
    for (const unsigned char character : source) {
        if (character == '=') break;
        const auto value = table[character];
        if (value < 0) {
            if (character == ' ' || character == '\t' || character == '\r' || character == '\n') continue;
            return {};
        }
        accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xffU));
        }
    }
    return bytes;
}

[[nodiscard]] std::vector<std::uint8_t> image_bytes(const cgltf_image& image,
                                                     const std::filesystem::path& modelPath) {
    if (image.buffer_view != nullptr && image.buffer_view->buffer != nullptr &&
        image.buffer_view->buffer->data != nullptr) {
        const auto* begin = static_cast<const std::uint8_t*>(image.buffer_view->buffer->data) +
                            image.buffer_view->offset;
        return {begin, begin + image.buffer_view->size};
    }
    if (image.uri == nullptr) return {};

    const std::string_view uri{image.uri};
    if (uri.starts_with("data:")) {
        const auto comma = uri.find(',');
        if (comma == std::string_view::npos) return {};
        const auto metadata = uri.substr(0, comma);
        if (metadata.ends_with(";base64")) return decode_base64(uri.substr(comma + 1));
        std::string decoded{uri.substr(comma + 1)};
        decoded.push_back('\0');
        const cgltf_size size = cgltf_decode_uri(decoded.data());
        return {decoded.begin(), decoded.begin() + static_cast<std::ptrdiff_t>(size)};
    }

    std::string decoded{uri};
    decoded.push_back('\0');
    cgltf_decode_uri(decoded.data());
    std::ifstream file(modelPath.parent_path() / decoded.c_str(), std::ios::binary | std::ios::ate);
    if (!file) return {};
    const auto size = file.tellg();
    if (size <= 0) return {};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!file) return {};
    return bytes;
}

[[nodiscard]] bool load_images(const cgltf_data& data, const std::filesystem::path& path,
                               Mesh& mesh) {
    mesh.images.reserve(data.images_count);
    for (cgltf_size i = 0; i < data.images_count; ++i) {
        const auto encoded = image_bytes(data.images[i], path);
        if (encoded.empty() || encoded.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return false;
        }
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()),
                                                &width, &height, &channels, STBI_rgb_alpha);
        if (pixels == nullptr || width <= 0 || height <= 0) {
            stbi_image_free(pixels);
            return false;
        }
        Mesh::Image result;
        result.width = static_cast<std::uint32_t>(width);
        result.height = static_cast<std::uint32_t>(height);
        const auto byteCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * STBI_rgb_alpha;
        result.rgbaPixels.assign(pixels, pixels + byteCount);
        stbi_image_free(pixels);
        mesh.images.push_back(std::move(result));
    }
    return true;
}

void load_materials(const cgltf_data& data, Mesh& mesh) {
    mesh.materials.reserve(data.materials_count + 1);
    for (cgltf_size i = 0; i < data.materials_count; ++i) {
        const cgltf_material& source = data.materials[i];
        PBRMaterial material{};
        if (source.has_pbr_metallic_roughness) {
            const auto& pbr = source.pbr_metallic_roughness;
            material.baseColor = {pbr.base_color_factor[0], pbr.base_color_factor[1],
                                  pbr.base_color_factor[2], pbr.base_color_factor[3]};
            material.metallic = pbr.metallic_factor;
            material.roughness = pbr.roughness_factor;
            material.baseColorTexture = image_index(data, pbr.base_color_texture);
            material.metallicRoughnessTexture = image_index(data, pbr.metallic_roughness_texture);
        }
        material.normalTexture = image_index(data, source.normal_texture);
        material.normalScale = source.normal_texture.texture == nullptr ? 1.0F : source.normal_texture.scale;
        material.aoTexture = image_index(data, source.occlusion_texture);
        material.aoStrength = source.occlusion_texture.texture == nullptr
                                      ? 1.0F : source.occlusion_texture.scale;
        material.alphaMode = source.alpha_mode == cgltf_alpha_mode_mask ? AlphaMode::Mask :
                             source.alpha_mode == cgltf_alpha_mode_blend ? AlphaMode::Blend :
                             AlphaMode::Opaque;
        material.alphaCutoff = source.alpha_mode == cgltf_alpha_mode_mask ? source.alpha_cutoff : 0.5F;
        material.doubleSided = source.double_sided != 0;
        mesh.materials.push_back(material);
    }
}

[[nodiscard]] std::vector<std::uint32_t> primitive_indices(const cgltf_primitive& primitive,
                                                            const std::size_t vertexCount) {
    const std::size_t count = primitive.indices == nullptr ? vertexCount : primitive.indices->count;
    std::vector<std::uint32_t> source;
    source.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t value = primitive.indices == nullptr ? i : cgltf_accessor_read_index(primitive.indices, i);
        if (value >= vertexCount || value > std::numeric_limits<std::uint32_t>::max()) return {};
        source.push_back(static_cast<std::uint32_t>(value));
    }

    std::vector<std::uint32_t> triangles;
    switch (primitive.type) {
        case cgltf_primitive_type_triangles:
            if (source.size() % 3 != 0) return {};
            return source;
        case cgltf_primitive_type_triangle_strip:
            if (source.size() < 3) return {};
            triangles.reserve((source.size() - 2) * 3);
            for (std::size_t i = 2; i < source.size(); ++i) {
                const std::uint32_t a = source[i - 2];
                const std::uint32_t b = source[i - 1];
                const std::uint32_t c = source[i];
                if (a == b || b == c || a == c) continue;
                if ((i & 1U) == 0) triangles.insert(triangles.end(), {a, b, c});
                else triangles.insert(triangles.end(), {b, a, c});
            }
            return triangles;
        case cgltf_primitive_type_triangle_fan:
            if (source.size() < 3) return {};
            triangles.reserve((source.size() - 2) * 3);
            for (std::size_t i = 2; i < source.size(); ++i) {
                if (source[0] != source[i - 1] && source[i - 1] != source[i] && source[0] != source[i]) {
                    triangles.insert(triangles.end(), {source[0], source[i - 1], source[i]});
                }
            }
            return triangles;
        default:
            return {};
    }
}

void generate_normals(Mesh& mesh, const std::size_t vertexStart, const std::size_t indexStart) {
    for (std::size_t i = indexStart; i + 2 < mesh.indices.size(); i += 3) {
        auto& a = mesh.vertices[mesh.indices[i]];
        auto& b = mesh.vertices[mesh.indices[i + 1]];
        auto& c = mesh.vertices[mesh.indices[i + 2]];
        const Vec3 normal = cross(b.position - a.position, c.position - a.position);
        if (normal.length() > 0.0F) {
            a.normal += normal;
            b.normal += normal;
            c.normal += normal;
        }
    }
    for (std::size_t i = vertexStart; i < mesh.vertices.size(); ++i) {
        auto& normal = mesh.vertices[i].normal;
        normal = normal.length() > 0.0F ? normal.normalized() : Vec3{0.0F, 1.0F, 0.0F};
    }
}

void generate_tangents(Mesh& mesh, const std::size_t vertexStart, const std::size_t indexStart) {
    std::vector<glm::vec3> tangents(mesh.vertices.size() - vertexStart);
    std::vector<glm::vec3> bitangents(mesh.vertices.size() - vertexStart);
    for (std::size_t i = indexStart; i + 2 < mesh.indices.size(); i += 3) {
        const std::array<std::uint32_t, 3> ids{mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]};
        const Vertex& a = mesh.vertices[ids[0]];
        const Vertex& b = mesh.vertices[ids[1]];
        const Vertex& c = mesh.vertices[ids[2]];
        const glm::vec3 edge1 = b.position.native() - a.position.native();
        const glm::vec3 edge2 = c.position.native() - a.position.native();
        const glm::vec2 uv1 = b.texCoord.native() - a.texCoord.native();
        const glm::vec2 uv2 = c.texCoord.native() - a.texCoord.native();
        const float determinant = uv1.x * uv2.y - uv1.y * uv2.x;
        if (std::abs(determinant) <= 1.0e-8F) continue;
        const float reciprocal = 1.0F / determinant;
        const glm::vec3 tangent = (edge1 * uv2.y - edge2 * uv1.y) * reciprocal;
        const glm::vec3 bitangent = (edge2 * uv1.x - edge1 * uv2.x) * reciprocal;
        for (const auto id : ids) {
            tangents[id - vertexStart] += tangent;
            bitangents[id - vertexStart] += bitangent;
        }
    }
    for (std::size_t i = vertexStart; i < mesh.vertices.size(); ++i) {
        Vertex& vertex = mesh.vertices[i];
        const glm::vec3 normal = vertex.normal.native();
        glm::vec3 tangent = tangents[i - vertexStart] - normal * glm::dot(normal, tangents[i - vertexStart]);
        if (glm::dot(tangent, tangent) <= 1.0e-12F) {
            const glm::vec3 axis = std::abs(normal.y) < 0.999F ? glm::vec3{0.0F, 1.0F, 0.0F}
                                                               : glm::vec3{1.0F, 0.0F, 0.0F};
            tangent = glm::cross(axis, normal);
        }
        tangent = glm::normalize(tangent);
        const float handedness = glm::dot(glm::cross(normal, tangent), bitangents[i - vertexStart]) < 0.0F
                                   ? -1.0F : 1.0F;
        vertex.tangent = Vec4{tangent.x, tangent.y, tangent.z, handedness};
    }
}

[[nodiscard]] bool append_primitive(const cgltf_data& data, const cgltf_node& node,
                                    const cgltf_primitive& primitive, Mesh& mesh) {
    const cgltf_accessor* positions = attribute(primitive, cgltf_attribute_type_position);
    if (positions == nullptr || positions->type != cgltf_type_vec3 || positions->count == 0 ||
        positions->count > std::numeric_limits<std::uint32_t>::max()) return false;

    const cgltf_accessor* normals = attribute(primitive, cgltf_attribute_type_normal);
    const cgltf_accessor* texCoords = attribute(primitive, cgltf_attribute_type_texcoord);
    const cgltf_accessor* colors = attribute(primitive, cgltf_attribute_type_color);
    const cgltf_accessor* sourceTangents = attribute(primitive, cgltf_attribute_type_tangent);
    if ((normals != nullptr && normals->count != positions->count) ||
        (texCoords != nullptr && texCoords->count != positions->count) ||
        (colors != nullptr && colors->count != positions->count) ||
        (sourceTangents != nullptr && sourceTangents->count != positions->count)) return false;

    const auto localIndices = primitive_indices(primitive, positions->count);
    if (localIndices.empty()) return false;
    const std::size_t vertexStart = mesh.vertices.size();
    const std::size_t indexStart = mesh.indices.size();
    if (vertexStart + positions->count > std::numeric_limits<std::uint32_t>::max()) return false;

    cgltf_float matrixValues[16]{};
    cgltf_node_transform_world(&node, matrixValues);
    const glm::mat4 transform = glm::make_mat4(matrixValues);
    const glm::mat3 linear{transform};
    const glm::mat3 normalTransform = glm::transpose(glm::inverse(linear));
    const float orientation = glm::determinant(linear) < 0.0F ? -1.0F : 1.0F;
    const auto materialIndex = primitive.material == nullptr
        ? static_cast<std::uint32_t>(data.materials_count)
        : static_cast<std::uint32_t>(primitive.material - data.materials);

    mesh.vertices.reserve(vertexStart + positions->count);
    for (cgltf_size i = 0; i < positions->count; ++i) {
        cgltf_float values[4]{};
        if (!cgltf_accessor_read_float(positions, i, values, 3)) return false;
        Vertex vertex{};
        const glm::vec4 position = transform * glm::vec4{values[0], values[1], values[2], 1.0F};
        vertex.position = Vec3{position.x, position.y, position.z};
        vertex.color = Vec3{1.0F, 1.0F, 1.0F};
        vertex.materialIndex = materialIndex;
        if (normals != nullptr) {
            if (!cgltf_accessor_read_float(normals, i, values, 3)) return false;
            const glm::vec3 normal = glm::normalize(normalTransform * glm::vec3{values[0], values[1], values[2]});
            vertex.normal = Vec3{normal};
        }
        if (texCoords != nullptr) {
            if (!cgltf_accessor_read_float(texCoords, i, values, 2)) return false;
            vertex.texCoord = Vec2{values[0], values[1]};
        }
        if (colors != nullptr) {
            if (!cgltf_accessor_read_float(colors, i, values, 4)) return false;
            vertex.color = Vec3{values[0], values[1], values[2]};
        }
        if (sourceTangents != nullptr) {
            if (!cgltf_accessor_read_float(sourceTangents, i, values, 4)) return false;
            const glm::vec3 tangent = glm::normalize(linear * glm::vec3{values[0], values[1], values[2]});
            vertex.tangent = Vec4{tangent.x, tangent.y, tangent.z, values[3] * orientation};
        }
        mesh.vertices.push_back(vertex);
    }

    mesh.indices.reserve(indexStart + localIndices.size());
    for (std::size_t i = 0; i < localIndices.size(); i += 3) {
        const std::uint32_t offset = static_cast<std::uint32_t>(vertexStart);
        if (orientation > 0.0F) {
            mesh.indices.insert(mesh.indices.end(), {offset + localIndices[i], offset + localIndices[i + 1],
                                                     offset + localIndices[i + 2]});
        } else {
            mesh.indices.insert(mesh.indices.end(), {offset + localIndices[i], offset + localIndices[i + 2],
                                                     offset + localIndices[i + 1]});
        }
    }
    if (normals == nullptr) generate_normals(mesh, vertexStart, indexStart);
    if (sourceTangents == nullptr && texCoords != nullptr) generate_tangents(mesh, vertexStart, indexStart);
    return true;
}

[[nodiscard]] bool append_node(const cgltf_data& data, const cgltf_node& node, Mesh& mesh) {
    if (node.mesh != nullptr) {
        for (cgltf_size i = 0; i < node.mesh->primitives_count; ++i) {
            if (!append_primitive(data, node, node.mesh->primitives[i], mesh)) return false;
        }
    }
    for (cgltf_size i = 0; i < node.children_count; ++i) {
        if (!append_node(data, *node.children[i], mesh)) return false;
    }
    return true;
}

} // namespace

std::shared_ptr<const Mesh> load_gltf_mesh(const std::filesystem::path& path) {
    cgltf_options options{};
    cgltf_data* parsed = nullptr;
    const std::string pathString = path.string();
    if (cgltf_parse_file(&options, pathString.c_str(), &parsed) != cgltf_result_success) return {};
    GltfData data{parsed, &cgltf_free};
    if ((data->file_type != cgltf_file_type_glb && data->file_type != cgltf_file_type_gltf) ||
        cgltf_load_buffers(&options, data.get(), pathString.c_str()) != cgltf_result_success ||
        cgltf_validate(data.get()) != cgltf_result_success) return {};

    Mesh mesh;
    load_materials(*data, mesh);
    // The last slot is the glTF default material used by primitives without one.
    mesh.materials.emplace_back();
    if (!load_images(*data, path, mesh)) return {};

    if (data->scene != nullptr) {
        for (cgltf_size i = 0; i < data->scene->nodes_count; ++i) {
            if (!append_node(*data, *data->scene->nodes[i], mesh)) return {};
        }
    } else {
        for (cgltf_size i = 0; i < data->nodes_count; ++i) {
            if (data->nodes[i].parent == nullptr && !append_node(*data, data->nodes[i], mesh)) return {};
        }
    }
    if (mesh.empty()) return {};
    mesh.sourcePath = path;
    return std::make_shared<const Mesh>(std::move(mesh));
}

} // namespace Engine::Assets

// NOLINTEND(readability-magic-numbers)
