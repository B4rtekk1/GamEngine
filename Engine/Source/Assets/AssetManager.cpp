#include "Engine/Assets/AssetManager.h"

#include "Engine/Renderer/Geometry/Mesh.h"

#include <stb_image.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <numeric>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <array>

namespace Engine::Assets {

namespace {

struct ObjIndex {
    int position{};
    int tex_coord{};
    int normal{};

    bool operator==(const ObjIndex&) const = default;
};

struct ObjIndexHash {
    std::size_t operator()(const ObjIndex& index) const noexcept {
        const auto combine = [](std::size_t seed, int value) {
            return seed ^ (std::hash<int>{}(value) + 0x9e3779b9u + (seed << 6u) + (seed >> 2u));
        };
        return combine(combine(std::hash<int>{}(index.position), index.tex_coord), index.normal);
    }
};

int resolve_obj_index(const int index, const std::size_t count) {
    if (index > 0) return index - 1;
    if (index < 0) return static_cast<int>(count) + index;
    return -1;
}

bool parse_obj_index(std::string_view token, ObjIndex& result) {
    result = {};
    std::size_t start = 0;
    int* fields[] = {&result.position, &result.tex_coord, &result.normal};
    for (int field = 0; field < 3 && start <= token.size(); ++field) {
        const auto end = token.find('/', start);
        const auto part = token.substr(start, end == std::string_view::npos ? token.size() - start : end - start);
        if (!part.empty()) {
            try {
                *fields[field] = std::stoi(std::string(part));
            } catch (...) {
                return false;
            }
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return result.position != 0;
}

std::shared_ptr<const Mesh> load_obj_mesh(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) return {};

    std::vector<Vec3> positions;
    std::vector<Vec2> tex_coords;
    std::vector<Vec3> normals;
    Mesh mesh;
    std::unordered_map<ObjIndex, std::uint32_t, ObjIndexHash> vertices;
    bool has_normals = true;
    std::string line;

    const auto add_vertex = [&](const ObjIndex& source) -> std::optional<std::uint32_t> {
        const ObjIndex resolved{
            resolve_obj_index(source.position, positions.size()),
            source.tex_coord == 0 ? -1 : resolve_obj_index(source.tex_coord, tex_coords.size()),
            source.normal == 0 ? -1 : resolve_obj_index(source.normal, normals.size())};
        if (resolved.position < 0 || static_cast<std::size_t>(resolved.position) >= positions.size() ||
            (resolved.tex_coord >= 0 && static_cast<std::size_t>(resolved.tex_coord) >= tex_coords.size()) ||
            (resolved.normal >= 0 && static_cast<std::size_t>(resolved.normal) >= normals.size())) return std::nullopt;

        if (const auto found = vertices.find(resolved); found != vertices.end()) return found->second;
        Vertex vertex;
        vertex.position = positions[resolved.position];
        vertex.color = Vec3{1.0f, 1.0f, 1.0f};
        if (resolved.tex_coord >= 0) vertex.texCoord = tex_coords[resolved.tex_coord];
        if (resolved.normal >= 0) vertex.normal = normals[resolved.normal];
        else has_normals = false;
        const auto index = static_cast<std::uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back(vertex);
        vertices.emplace(resolved, index);
        return index;
    };

    while (std::getline(file, line)) {
        std::istringstream stream(line);
        std::string command;
        stream >> command;
        if (command.empty() || command[0] == '#') continue;
        if (command == "v") {
            float x, y, z;
            if (!(stream >> x >> y >> z)) return {};
            positions.emplace_back(x, y, z);
        } else if (command == "vt") {
            float u, v;
            if (!(stream >> u >> v)) return {};
            tex_coords.emplace_back(u, v);
        } else if (command == "vn") {
            float x, y, z;
            if (!(stream >> x >> y >> z)) return {};
            normals.emplace_back(x, y, z);
        } else if (command == "f") {
            std::vector<std::uint32_t> face;
            std::string token;
            while (stream >> token) {
                ObjIndex source;
                if (!parse_obj_index(token, source)) return {};
                const auto index = add_vertex(source);
                if (!index) return {};
                face.push_back(*index);
            }
            if (face.size() < 3) return {};
            for (std::size_t i = 1; i + 1 < face.size(); ++i) {
                mesh.indices.insert(mesh.indices.end(), {face[0], face[i], face[i + 1]});
            }
        }
    }

    if (mesh.empty()) return {};
    if (!has_normals) {
        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            auto& a = mesh.vertices[mesh.indices[i]];
            auto& b = mesh.vertices[mesh.indices[i + 1]];
            auto& c = mesh.vertices[mesh.indices[i + 2]];
            const auto normal = cross(b.position - a.position, c.position - a.position).normalized();
            a.normal += normal;
            b.normal += normal;
            c.normal += normal;
        }
        for (auto& vertex : mesh.vertices) {
            if (vertex.normal.length() > 0.0f) vertex.normal = vertex.normal.normalized();
            else vertex.normal = Vec3{0.0f, 1.0f, 0.0f};
        }
    }
    return std::make_shared<const Mesh>(std::move(mesh));
}

// GLB is deliberately parsed here instead of adding a third-party dependency. The
// importer covers the geometry subset used by Mesh: POSITION, NORMAL, TEXCOORD_0
// and indices from one or more mesh primitives.
class JsonValue {
public:
    using Object = std::unordered_map<std::string, JsonValue>;
    using Array = std::vector<JsonValue>;
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, Object, Array>;

    Storage value;
    const JsonValue* find(std::string_view key) const {
        if (const auto* object = std::get_if<Object>(&value)) {
            if (const auto it = object->find(std::string(key)); it != object->end()) return &it->second;
        }
        return nullptr;
    }
    std::optional<double> number() const {
        if (const auto* number = std::get_if<double>(&value)) return *number;
        return std::nullopt;
    }
    std::optional<bool> boolean() const {
        if (const auto* boolean = std::get_if<bool>(&value)) return *boolean;
        return std::nullopt;
    }
    const Array* array() const { return std::get_if<Array>(&value); }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}
    std::optional<JsonValue> parse() {
        skip_space();
        auto result = parse_value();
        skip_space();
        return result && position_ == text_.size() ? result : std::nullopt;
    }

private:
    void skip_space() { while (position_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[position_]))) ++position_; }
    std::optional<JsonValue> parse_value() {
        skip_space();
        if (position_ >= text_.size()) return std::nullopt;
        if (text_[position_] == '{') return parse_object();
        if (text_[position_] == '[') return parse_array();
        if (text_[position_] == '"') { auto string = parse_string(); if (!string) return std::nullopt; return std::optional<JsonValue>{JsonValue{std::move(*string)}}; }
        if (text_.substr(position_, 4) == "true") { position_ += 4; return JsonValue{true}; }
        if (text_.substr(position_, 5) == "false") { position_ += 5; return JsonValue{false}; }
        if (text_.substr(position_, 4) == "null") { position_ += 4; return JsonValue{nullptr}; }
        const auto start = position_;
        if (text_[position_] == '-') ++position_;
        while (position_ < text_.size() && (std::isdigit(static_cast<unsigned char>(text_[position_])) ||
                                             text_[position_] == '.' || text_[position_] == 'e' || text_[position_] == 'E' ||
                                             text_[position_] == '+' || text_[position_] == '-')) ++position_;
        try { return start == position_ ? std::nullopt : std::optional<JsonValue>{JsonValue{std::stod(std::string(text_.substr(start, position_ - start)))}}; }
        catch (...) { return std::nullopt; }
    }
    std::optional<std::string> parse_string() {
        if (position_ >= text_.size() || text_[position_++] != '"') return std::nullopt;
        std::string result;
        while (position_ < text_.size()) {
            const char c = text_[position_++];
            if (c == '"') return result;
            if (c == '\\') {
                if (position_ >= text_.size()) return std::nullopt;
                const char escaped = text_[position_++];
                switch (escaped) { case '"': case '\\': case '/': result += escaped; break; case 'b': result += '\b'; break; case 'f': result += '\f'; break; case 'n': result += '\n'; break; case 'r': result += '\r'; break; case 't': result += '\t'; break; default: return std::nullopt; }
            } else result += c;
        }
        return std::nullopt;
    }
    std::optional<JsonValue> parse_object() {
        ++position_; JsonValue::Object object; skip_space();
        if (position_ < text_.size() && text_[position_] == '}') { ++position_; return JsonValue{std::move(object)}; }
        while (position_ < text_.size()) {
            skip_space(); auto key = parse_string(); if (!key) return std::nullopt;
            skip_space(); if (position_ >= text_.size() || text_[position_++] != ':') return std::nullopt;
            auto value = parse_value(); if (!value) return std::nullopt;
            object.emplace(std::move(*key), std::move(*value)); skip_space();
            if (position_ < text_.size() && text_[position_] == '}') { ++position_; return JsonValue{std::move(object)}; }
            if (position_ >= text_.size() || text_[position_++] != ',') return std::nullopt;
        }
        return std::nullopt;
    }
    std::optional<JsonValue> parse_array() {
        ++position_; JsonValue::Array array; skip_space();
        if (position_ < text_.size() && text_[position_] == ']') { ++position_; return JsonValue{std::move(array)}; }
        while (position_ < text_.size()) {
            auto value = parse_value(); if (!value) return std::nullopt;
            array.push_back(std::move(*value)); skip_space();
            if (position_ < text_.size() && text_[position_] == ']') { ++position_; return JsonValue{std::move(array)}; }
            if (position_ >= text_.size() || text_[position_++] != ',') return std::nullopt;
        }
        return std::nullopt;
    }
    std::string_view text_; std::size_t position_{};
};

void generate_missing_tangents(Mesh& mesh) {
    std::vector<glm::vec3> tangentSums(mesh.vertices.size(), glm::vec3{});
    std::vector<glm::vec3> bitangentSums(mesh.vertices.size(), glm::vec3{});
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const std::uint32_t ia = mesh.indices[i], ib = mesh.indices[i + 1], ic = mesh.indices[i + 2];
        const Vertex& a = mesh.vertices[ia]; const Vertex& b = mesh.vertices[ib]; const Vertex& c = mesh.vertices[ic];
        const glm::vec3 edge1 = b.position.native() - a.position.native();
        const glm::vec3 edge2 = c.position.native() - a.position.native();
        const glm::vec2 deltaUv1 = b.texCoord.native() - a.texCoord.native();
        const glm::vec2 deltaUv2 = c.texCoord.native() - a.texCoord.native();
        const float determinant = deltaUv1.x * deltaUv2.y - deltaUv1.y * deltaUv2.x;
        if (std::abs(determinant) < 1e-8f) continue;
        const float reciprocal = 1.0f / determinant;
        const glm::vec3 tangent = reciprocal * (deltaUv2.y * edge1 - deltaUv1.y * edge2);
        const glm::vec3 bitangent = reciprocal * (-deltaUv2.x * edge1 + deltaUv1.x * edge2);
        tangentSums[ia] += tangent; tangentSums[ib] += tangent; tangentSums[ic] += tangent;
        bitangentSums[ia] += bitangent; bitangentSums[ib] += bitangent; bitangentSums[ic] += bitangent;
    }
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        Vertex& vertex = mesh.vertices[i];
        if (glm::length(glm::vec3(vertex.tangent.native())) > 1e-6f) continue;
        const glm::vec3 normal = glm::normalize(vertex.normal.native());
        glm::vec3 tangent = tangentSums[i] - normal * glm::dot(normal, tangentSums[i]);
        if (glm::length(tangent) < 1e-6f) {
            const glm::vec3 axis = std::abs(normal.y) < 0.999f ? glm::vec3{0, 1, 0} : glm::vec3{1, 0, 0};
            tangent = glm::cross(axis, normal);
        }
        tangent = glm::normalize(tangent);
        const float handedness = glm::dot(glm::cross(normal, tangent), bitangentSums[i]) < 0.0f ? -1.0f : 1.0f;
        vertex.tangent = Vec4{tangent.x, tangent.y, tangent.z, handedness};
    }
}

std::shared_ptr<const Mesh> load_glb_mesh(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    const auto size = file.tellg(); if (size < 20) return {};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size)); file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()), size);
    auto read_u32 = [&](std::size_t offset) { std::uint32_t value{}; std::memcpy(&value, bytes.data() + offset, sizeof(value)); return value; };
    if (read_u32(0) != 0x46546C67u || read_u32(4) != 2u || read_u32(8) != bytes.size()) return {};
    std::string json; std::vector<std::uint8_t> buffer; std::size_t offset = 12;
    while (offset + 8 <= bytes.size()) {
        const auto chunk_size = read_u32(offset); const auto chunk_type = read_u32(offset + 4); offset += 8;
        if (offset + chunk_size > bytes.size()) return {};
        if (chunk_type == 0x4E4F534Au) json.assign(reinterpret_cast<const char*>(bytes.data() + offset), chunk_size);
        else if (chunk_type == 0x004E4942u) buffer.assign(bytes.begin() + offset, bytes.begin() + offset + chunk_size);
        offset += chunk_size;
    }
    const auto document = JsonParser(json).parse(); if (!document) return {};
    const auto* buffers = document->find("buffers"); const auto* views = document->find("bufferViews"); const auto* accessors = document->find("accessors"); const auto* meshes = document->find("meshes");
    if (!buffers || !views || !accessors || !meshes || !buffers->array() || !views->array() || !accessors->array() || !meshes->array() || buffers->array()->empty()) return {};
    const auto& view_list = *views->array(); const auto& accessor_list = *accessors->array(); const auto& mesh_list = *meshes->array(); Mesh mesh;
    const auto texture_source = [&](const JsonValue* texture) -> std::int32_t {
        if (!texture || !texture->number()) return -1;
        const auto* textures = document->find("textures");
        if (!textures || !textures->array() || *texture->number() >= textures->array()->size()) return -1;
        const auto* source = (*textures->array())[static_cast<std::size_t>(*texture->number())].find("source");
        return source && source->number() ? static_cast<std::int32_t>(*source->number()) : -1;
    };
    if (const auto* image_list = document->find("images"); image_list && image_list->array()) {
        mesh.images.resize(image_list->array()->size());
        for (std::size_t image_index = 0; image_index < image_list->array()->size(); ++image_index) {
            const auto* view = (*image_list->array())[image_index].find("bufferView");
            if (!view || !view->number() || *view->number() >= view_list.size()) continue;
            const auto& image_view = view_list[static_cast<std::size_t>(*view->number())];
            const auto* image_offset = image_view.find("byteOffset");
            const auto* image_length = image_view.find("byteLength");
            if (!image_length || !image_length->number()) continue;
            const auto offset = image_offset && image_offset->number() ? static_cast<std::size_t>(*image_offset->number()) : 0u;
            const auto length = static_cast<std::size_t>(*image_length->number());
            if (offset > buffer.size() || length > buffer.size() - offset) continue;
            int width{}, height{}, channels{};
            stbi_uc* pixels = stbi_load_from_memory(buffer.data() + offset, static_cast<int>(length), &width, &height, &channels, STBI_rgb_alpha);
            if (!pixels || width <= 0 || height <= 0) { stbi_image_free(pixels); continue; }
            auto& image = mesh.images[image_index];
            image.width = static_cast<uint32_t>(width); image.height = static_cast<uint32_t>(height);
            image.rgbaPixels.assign(pixels, pixels + static_cast<std::size_t>(width) * height * STBI_rgb_alpha);
            stbi_image_free(pixels);
        }
    }
    // Preserve the material table instead of flattening every GLB primitive
    // into the renderer's default material. The Vulkan upload path can then
    // bind the correct material for each vertex/primitive.
    if (const auto* material_list = document->find("materials"); material_list && material_list->array()) {
        for (const auto& material_value : *material_list->array()) {
            PBRMaterial material{};
            if (const auto* pbr = material_value.find("pbrMetallicRoughness")) {
                if (const auto* factor = pbr->find("baseColorFactor"); factor && factor->array() && factor->array()->size() >= 3) {
                    const auto& values = *factor->array();
                    const auto number = [](const JsonValue& value, const float fallback) {
                        return value.number().has_value() ? static_cast<float>(*value.number()) : fallback;
                    };
                    material.baseColor = Math::Color{
                        number(values[0], 1.0f), number(values[1], 1.0f),
                        number(values[2], 1.0f), values.size() > 3 ? number(values[3], 1.0f) : 1.0f};
                }
                if (const auto* factor = pbr->find("metallicFactor"); factor && factor->number()) material.metallic = static_cast<float>(*factor->number());
                if (const auto* factor = pbr->find("roughnessFactor"); factor && factor->number()) material.roughness = static_cast<float>(*factor->number());
                if (const auto* texture = pbr->find("baseColorTexture")) material.baseColorTexture = texture_source(texture->find("index"));
                if (const auto* texture = pbr->find("metallicRoughnessTexture")) material.metallicRoughnessTexture = texture_source(texture->find("index"));
            }
            if (const auto* texture = material_value.find("normalTexture")) {
                material.normalTexture = texture_source(texture->find("index"));
                if (const auto* scale = texture->find("scale"); scale && scale->number()) material.normalScale = static_cast<float>(*scale->number());
            }
            if (const auto* alpha_mode = material_value.find("alphaMode"); alpha_mode && std::get_if<std::string>(&alpha_mode->value)) material.alphaBlend = *std::get_if<std::string>(&alpha_mode->value) == "BLEND";
            if (const auto* double_sided = material_value.find("doubleSided")) material.doubleSided = double_sided->boolean().value_or(false);
            if (const auto* cutoff = material_value.find("alphaCutoff"); cutoff && cutoff->number()) material.alphaCutoff = static_cast<float>(*cutoff->number());
            mesh.materials.push_back(material);
        }
    }
    auto accessor_bytes = [&](std::size_t index, std::size_t components, std::vector<float>& output) -> bool {
        if (index >= accessor_list.size()) return false; const auto* accessor = accessor_list[index].find("bufferView"); const auto* count = accessor_list[index].find("count"); const auto* type = accessor_list[index].find("type");
        if (!accessor || !count || !type || !count->number() || !type || !std::get_if<std::string>(&type->value)) return false; const auto view_index = accessor->number(); if (!view_index || *view_index >= view_list.size()) return false;
        const auto view_id = static_cast<std::size_t>(*view_index);
        const auto* buffer_index = view_list[view_id].find("buffer"); const auto* view_offset = view_list[view_id].find("byteOffset"); const auto* view_length = view_list[view_id].find("byteLength"); const auto* component = accessor_list[index].find("componentType"); const auto* accessor_offset = accessor_list[index].find("byteOffset");
        if (!view_length || !component || !view_length->number() || !component->number()) return false;
        const auto component_type = static_cast<std::uint32_t>(*component->number());
        const auto component_size = component_type == 5120 || component_type == 5121 ? 1u :
                                    component_type == 5122 || component_type == 5123 ? 2u :
                                    component_type == 5125 || component_type == 5126 ? 4u : 0u;
        if (!component_size) return false;
        const auto* normalized_value = accessor_list[index].find("normalized");
        const bool normalized = normalized_value && normalized_value->boolean().value_or(false);
        if (normalized && (component_type == 5125 || component_type == 5126)) return false;
        // A GLB embeds its binary data in buffer zero.  Reject another buffer
        // instead of accidentally interpreting bytes from the BIN chunk.
        if (buffer_index && (!buffer_index->number() || *buffer_index->number() != 0.0)) return false;
        const auto expected = components == 4 ? "VEC4" : (components == 3 ? "VEC3" : (components == 2 ? "VEC2" : "SCALAR")); if (*std::get_if<std::string>(&type->value) != expected) return false;
        const auto stride = view_list[view_id].find("byteStride"); const auto step = static_cast<std::size_t>(stride && stride->number() ? *stride->number() : component_size * components);
        const auto view_begin = static_cast<std::size_t>(view_offset && view_offset->number() ? *view_offset->number() : 0);
        const auto relative_offset = static_cast<std::size_t>(accessor_offset && accessor_offset->number() ? *accessor_offset->number() : 0);
        if (relative_offset > std::numeric_limits<std::size_t>::max() - view_begin) return false;
        const auto begin = view_begin + relative_offset;
        const auto item_count = static_cast<std::size_t>(*count->number());
        const auto view_size = static_cast<std::size_t>(*view_length->number());
        const auto item_size = static_cast<std::size_t>(component_size) * components;
        if (item_count == 0 || step < item_size || relative_offset > view_size || item_size > view_size - relative_offset ||
            view_begin > buffer.size() || view_size > buffer.size() - view_begin ||
            (item_count - 1) > (view_size - relative_offset - item_size) / step) return false;
        output.resize(item_count * components);
        for (std::size_t i = 0; i < item_count; ++i) for (std::size_t c = 0; c < components; ++c) {
            const auto* p = buffer.data() + begin + i * step + c * component_size;
            float value{};
            if (component_type == 5126) std::memcpy(&value, p, sizeof(value));
            else if (component_type == 5120) { std::int8_t integer{}; std::memcpy(&integer, p, sizeof(integer)); value = normalized ? std::max(static_cast<float>(integer) / 127.0f, -1.0f) : static_cast<float>(integer); }
            else if (component_type == 5121) { const auto integer = *p; value = normalized ? static_cast<float>(integer) / 255.0f : static_cast<float>(integer); }
            else if (component_type == 5122) { std::int16_t integer{}; std::memcpy(&integer, p, sizeof(integer)); value = normalized ? std::max(static_cast<float>(integer) / 32767.0f, -1.0f) : static_cast<float>(integer); }
            else if (component_type == 5123) { std::uint16_t integer{}; std::memcpy(&integer, p, sizeof(integer)); value = normalized ? static_cast<float>(integer) / 65535.0f : static_cast<float>(integer); }
            else { std::uint32_t integer{}; std::memcpy(&integer, p, sizeof(integer)); value = static_cast<float>(integer); }
            output[i * components + c] = value;
        }
        return true;
    };
    // glTF meshes are definitions, not scene objects.  Blender exports the
    // objects in `scenes`/`nodes`, where a mesh may be instantiated more than
    // once and receives its world transform from the whole node hierarchy.
    const auto append_mesh = [&](const std::size_t mesh_index, const glm::mat4& world) -> bool {
        if (mesh_index >= mesh_list.size()) return false;
        const float determinant = glm::determinant(glm::mat3(world));
        if (std::abs(determinant) < 1e-8f) return false;
        const glm::mat3 normal_matrix = glm::transpose(glm::inverse(glm::mat3(world)));
        const auto& mesh_value = mesh_list[mesh_index]; const auto* primitives = mesh_value.find("primitives"); if (!primitives || !primitives->array()) return true; for (const auto& primitive : *primitives->array()) {
        const auto* attributes = primitive.find("attributes"); if (!attributes) continue; const auto* position = attributes->find("POSITION"); if (!position || !position->number()) return {};
        const auto primitive_material = primitive.find("material");
        const auto material_index = primitive_material && primitive_material->number()
            ? static_cast<std::uint32_t>(*primitive_material->number()) : 0u;
        if (!mesh.materials.empty() && material_index >= mesh.materials.size()) return {};
        std::vector<float> positions, normals, texcoords, tangents; if (!accessor_bytes(*position->number(), 3, positions)) return {}; const auto* normal = attributes->find("NORMAL"); const auto* texcoord = attributes->find("TEXCOORD_0"); const auto* tangent = attributes->find("TANGENT"); if (normal && normal->number()) accessor_bytes(*normal->number(), 3, normals); if (texcoord && texcoord->number()) accessor_bytes(*texcoord->number(), 2, texcoords); if (tangent && tangent->number()) accessor_bytes(*tangent->number(), 4, tangents);
        const auto base = static_cast<std::uint32_t>(mesh.vertices.size()); for (std::size_t i = 0; i < positions.size() / 3; ++i) { Vertex vertex; const glm::vec3 position_value{positions[i*3], positions[i*3+1], positions[i*3+2]}; vertex.position = Vec3{glm::vec3(world * glm::vec4(position_value, 1.0f))}; vertex.color = {1,1,1}; vertex.materialIndex = material_index; if (i*3+2 < normals.size()) vertex.normal = Vec3{glm::normalize(normal_matrix * glm::vec3{normals[i*3], normals[i*3+1], normals[i*3+2]})}; if (i*2+1 < texcoords.size()) vertex.texCoord = {texcoords[i*2], texcoords[i*2+1]}; if (i*4+3 < tangents.size()) { const auto tangent_value = glm::normalize(glm::mat3(world) * glm::vec3{tangents[i*4], tangents[i*4+1], tangents[i*4+2]}); vertex.tangent = {tangent_value.x, tangent_value.y, tangent_value.z, tangents[i*4+3] * (determinant < 0.0f ? -1.0f : 1.0f)}; } mesh.vertices.push_back(vertex); }
        std::vector<std::uint32_t> primitive_indices;
        const auto* indices = primitive.find("indices");
        if (indices && indices->number()) {
            std::vector<float> values; if (!accessor_bytes(*indices->number(), 1, values)) return {};
            primitive_indices.reserve(values.size());
            for (const float value : values) {
                if (value < 0.0f || value != std::floor(value) || value >= positions.size() / 3) return {};
                primitive_indices.push_back(static_cast<std::uint32_t>(value));
            }
        } else {
            primitive_indices.resize(static_cast<std::uint32_t>(positions.size() / 3));
            std::iota(primitive_indices.begin(), primitive_indices.end(), 0u);
        }
        const auto* mode_value = primitive.find("mode");
        const auto mode = mode_value && mode_value->number() ? static_cast<std::uint32_t>(*mode_value->number()) : 4u;
        const auto add_triangle = [&](const std::uint32_t a, const std::uint32_t b, const std::uint32_t c) {
            if (determinant < 0.0f) mesh.indices.insert(mesh.indices.end(), {base + a, base + c, base + b});
            else mesh.indices.insert(mesh.indices.end(), {base + a, base + b, base + c});
        };
        if (mode == 4u) {
            if (primitive_indices.size() % 3 != 0) return {};
            for (std::size_t i = 0; i < primitive_indices.size(); i += 3) add_triangle(primitive_indices[i], primitive_indices[i + 1], primitive_indices[i + 2]);
        } else if (mode == 5u) {
            for (std::size_t i = 2; i < primitive_indices.size(); ++i)
                if (i % 2 == 0) add_triangle(primitive_indices[i - 2], primitive_indices[i - 1], primitive_indices[i]);
                else add_triangle(primitive_indices[i - 1], primitive_indices[i - 2], primitive_indices[i]);
        } else if (mode == 6u) {
            for (std::size_t i = 2; i < primitive_indices.size(); ++i) add_triangle(primitive_indices[0], primitive_indices[i - 1], primitive_indices[i]);
        } else return false;
    } return true; };
    const auto node_transform = [](const JsonValue& node, glm::mat4& transform) -> bool {
        transform = glm::mat4{1.0f};
        if (const auto* matrix = node.find("matrix")) {
            if (!matrix->array() || matrix->array()->size() != 16) return false;
            for (std::size_t i = 0; i < 16; ++i) { const auto value = (*matrix->array())[i].number(); if (!value) return false; transform[i / 4][i % 4] = static_cast<float>(*value); }
            return true;
        }
        glm::vec3 translation{}; glm::vec3 scale{1.0f}; glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        const auto read_vec = [](const JsonValue* value, const std::size_t size, float* output) -> bool { if (!value) return true; if (!value->array() || value->array()->size() != size) return false; for (std::size_t i = 0; i < size; ++i) { const auto number = (*value->array())[i].number(); if (!number) return false; output[i] = static_cast<float>(*number); } return true; };
        if (!read_vec(node.find("translation"), 3, &translation.x) || !read_vec(node.find("scale"), 3, &scale.x) || !read_vec(node.find("rotation"), 4, &rotation.x)) return false;
        rotation = glm::normalize(rotation);
        transform = glm::translate(glm::mat4{1.0f}, translation) * glm::mat4_cast(rotation) * glm::scale(glm::mat4{1.0f}, scale);
        return true;
    };
    const auto* nodes = document->find("nodes");
    bool imported_scene = false;
    if (nodes && nodes->array()) {
        const auto& node_list = *nodes->array();
        const auto append_node = [&](auto&& self, const std::size_t node_index, const glm::mat4& parent, std::vector<bool>& visiting) -> bool {
            if (node_index >= node_list.size() || visiting[node_index]) return false;
            visiting[node_index] = true; glm::mat4 local;
            if (!node_transform(node_list[node_index], local)) return false;
            const glm::mat4 world = parent * local;
            if (const auto* node_mesh = node_list[node_index].find("mesh"); node_mesh && node_mesh->number() && !append_mesh(static_cast<std::size_t>(*node_mesh->number()), world)) return false;
            if (const auto* children = node_list[node_index].find("children"); children) { if (!children->array()) return false; for (const auto& child : *children->array()) { const auto child_index = child.number(); if (!child_index || *child_index < 0.0 || !self(self, static_cast<std::size_t>(*child_index), world, visiting)) return false; } }
            visiting[node_index] = false; return true;
        };
        const auto* scenes = document->find("scenes"); const auto* selected_scene = document->find("scene");
        if (scenes && scenes->array() && !scenes->array()->empty()) {
            const auto scene_index = selected_scene && selected_scene->number() ? static_cast<std::size_t>(*selected_scene->number()) : 0u;
            if (scene_index >= scenes->array()->size()) return {};
            const auto* roots = (*scenes->array())[scene_index].find("nodes"); if (!roots || !roots->array()) return {};
            std::vector<bool> visiting(node_list.size());
            for (const auto& root : *roots->array()) { const auto root_index = root.number(); if (!root_index || *root_index < 0.0 || !append_node(append_node, static_cast<std::size_t>(*root_index), glm::mat4{1.0f}, visiting)) return {}; }
            imported_scene = true;
        }
    }
    // Some tools generate mesh-only GLBs. Keep that useful subset as a
    // compatibility fallback when no scene graph is supplied.
    if (!imported_scene) for (std::size_t i = 0; i < mesh_list.size(); ++i) if (!append_mesh(i, glm::mat4{1.0f})) return {};
    if (mesh.empty()) return {};
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        auto& a = mesh.vertices[mesh.indices[i]];
        auto& b = mesh.vertices[mesh.indices[i + 1]];
        auto& c = mesh.vertices[mesh.indices[i + 2]];
        const auto normal = cross(b.position - a.position, c.position - a.position).normalized();
        if (a.normal.length() == 0) a.normal = normal;
        if (b.normal.length() == 0) b.normal = normal;
        if (c.normal.length() == 0) c.normal = normal;
    }
    generate_missing_tangents(mesh);
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

std::filesystem::path AssetManager::resolve(const std::filesystem::path& path) const {
    if (path.is_absolute() || asset_root_.empty()) return path.lexically_normal();
    return (asset_root_ / path).lexically_normal();
}

std::string AssetManager::normalize_path(const std::filesystem::path& path) {
    auto result = path.lexically_normal().generic_string();
    std::ranges::transform(result, result.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::string AssetManager::make_key(const std::filesystem::path& path) const {
    return normalize_path(resolve(path));
}

AssetId AssetManager::make_id(std::string_view value) noexcept {
    // FNV-1a is stable between runs and platforms, unlike std::hash.
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto c : value) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 1099511628211ull;
    }
    return hash == 0 ? 1 : hash;
}

std::size_t AssetManager::CacheKeyHash::operator()(const CacheKey& key) const noexcept {
    const auto a = std::hash<AssetId>{}(key.id);
    const auto b = key.type.hash_code();
    const auto c = std::hash<std::string>{}(key.path);
    return (a ^ (b + 0x9e3779b9u + (a << 6u) + (a >> 2u))) ^
           (c + 0x9e3779b9u + (a << 6u) + (a >> 2u));
}

void AssetManager::report(const std::string& message) const {
    ErrorHandler handler;
    {
        std::scoped_lock lock(mutex_);
        handler = error_handler_;
    }
    if (handler) handler(message);
}

void AssetManager::unload_unused() {
    std::scoped_lock lock(mutex_);
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (it->second.value.use_count() == 1) it = cache_.erase(it);
        else ++it;
    }
}

void AssetManager::clear() {
    std::scoped_lock lock(mutex_);
    cache_.clear();
}

bool AssetManager::contains(AssetId id, std::type_index type) const {
    std::scoped_lock lock(mutex_);
    return std::ranges::any_of(cache_, [id, type](const auto& entry) {
        return entry.first.id == id && entry.first.type == type;
    });
}

std::size_t AssetManager::size() const {
    std::scoped_lock lock(mutex_);
    return cache_.size();
}

void register_default_asset_loaders(AssetManager& manager) {
    const auto text_loader = [](const std::filesystem::path& path, const AssetMetadata&) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return std::shared_ptr<const TextAsset>{};
        std::ostringstream stream;
        stream << file.rdbuf();
        return std::make_shared<const TextAsset>(TextAsset{stream.str()});
    };

    const auto binary_loader = [](const std::filesystem::path& path, const AssetMetadata&) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return std::shared_ptr<const BinaryAsset>{};
        const auto size = file.tellg();
        if (size < 0) return std::shared_ptr<const BinaryAsset>{};
        BinaryAsset asset;
        asset.bytes.resize(static_cast<std::size_t>(size));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(asset.bytes.data()), size);
        return std::make_shared<const BinaryAsset>(std::move(asset));
    };

    manager.register_loader<TextAsset>(AssetType::Text, text_loader);
    manager.register_loader<ShaderAsset>(AssetType::Shader, [text_loader](const auto& path, const auto& metadata) {
        auto text = text_loader(path, metadata);
        if (!text) return std::shared_ptr<const ShaderAsset>{};
        return std::make_shared<const ShaderAsset>(ShaderAsset{text->text, "main"});
    });
    manager.register_loader<BinaryAsset>(AssetType::Binary, binary_loader);
    manager.register_loader<TextureAsset>(AssetType::Texture2D, [](const auto& path, const auto&) {
        int width{}, height{}, channels{};
        stbi_uc* pixels = stbi_load(path.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
        if (!pixels || width <= 0 || height <= 0) {
            stbi_image_free(pixels);
            return std::shared_ptr<const TextureAsset>{};
        }
        TextureAsset texture;
        texture.width = static_cast<std::uint32_t>(width);
        texture.height = static_cast<std::uint32_t>(height);
        texture.rgbaPixels.assign(pixels, pixels + static_cast<std::size_t>(width) * height * STBI_rgb_alpha);
        stbi_image_free(pixels);
        return std::make_shared<const TextureAsset>(std::move(texture));
    });
    manager.register_loader<PBRMaterial>(AssetType::Material, [](const auto& path, const auto&) {
        std::ifstream file(path);
        if (!file) return std::shared_ptr<const PBRMaterial>{};
        PBRMaterial material{};
        float red{}, green{}, blue{}, alpha{};
        if (!(file >> red >> green >> blue >> alpha
                    >> material.metallic >> material.roughness >> material.ambientOcclusion
                    >> material.baseColorTexture >> material.metallicRoughnessTexture
                    >> material.normalTexture >> material.normalScale
                    >> material.alphaBlend >> material.doubleSided >> material.alphaCutoff)) {
            return std::shared_ptr<const PBRMaterial>{};
        }
        material.baseColor = Math::Color{red, green, blue, alpha};
        return std::make_shared<const PBRMaterial>(std::move(material));
    });
    manager.register_loader<Mesh>(AssetType::Mesh, [](const auto& path, const auto&) {
        const auto extension = path.extension().string();
        if (extension == ".obj" || extension == ".OBJ") return load_obj_mesh(path);
        if (extension == ".glb" || extension == ".GLB") return load_glb_mesh(path);
        return std::shared_ptr<const Mesh>{};
    });
}

}
