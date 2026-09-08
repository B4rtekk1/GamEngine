#include "Engine/Renderer/ShaderGraph/ShaderGraphSerializer.h"

#include <fstream>
#include <iomanip>
#include <string_view>
#include <stdexcept>

namespace Engine {
    namespace {
        [[noreturn]] void invalid(const std::filesystem::path& path) {
            throw std::runtime_error("Invalid shader graph: " + path.string());
        }

        [[nodiscard]] std::string_view shaderNodeTypeId(const ShaderNodeType type) {
            switch (type) {
                case ShaderNodeType::Float: return "float";
                case ShaderNodeType::Vector2: return "vector2";
                case ShaderNodeType::Vector3: return "vector3";
                case ShaderNodeType::Vector4: return "vector4";
                case ShaderNodeType::Property: return "property";
                case ShaderNodeType::Add: return "add";
                case ShaderNodeType::Subtract: return "subtract";
                case ShaderNodeType::Multiply: return "multiply";
                case ShaderNodeType::Divide: return "divide";
                case ShaderNodeType::Lerp: return "lerp";
                case ShaderNodeType::Clamp: return "clamp";
                case ShaderNodeType::Saturate: return "saturate";
                case ShaderNodeType::OneMinus: return "one_minus";
                case ShaderNodeType::Sin: return "sin";
                case ShaderNodeType::Cos: return "cos";
                case ShaderNodeType::Dot: return "dot";
                case ShaderNodeType::Normalize: return "normalize";
                case ShaderNodeType::Length: return "length";
                case ShaderNodeType::Split: return "split";
                case ShaderNodeType::Combine: return "combine";
                case ShaderNodeType::Texture2D: return "texture2d";
                case ShaderNodeType::SampleTexture2D: return "sample_texture2d";
                case ShaderNodeType::UV: return "uv";
                case ShaderNodeType::Time: return "time";
                case ShaderNodeType::Normal: return "normal";
                case ShaderNodeType::ViewDirection: return "view_direction";
                case ShaderNodeType::Fresnel: return "fresnel";
                case ShaderNodeType::SurfaceOutput: return "surface_output";
            }
            throw std::runtime_error("Unknown shader node type");
        }

        [[nodiscard]] std::optional<ShaderNodeType> shaderNodeTypeFromId(const std::string_view id) {
            static constexpr std::pair<std::string_view, ShaderNodeType> nodeTypes[] = {
                {"float", ShaderNodeType::Float}, {"vector2", ShaderNodeType::Vector2},
                {"vector3", ShaderNodeType::Vector3}, {"vector4", ShaderNodeType::Vector4},
                {"property", ShaderNodeType::Property}, {"add", ShaderNodeType::Add},
                {"subtract", ShaderNodeType::Subtract}, {"multiply", ShaderNodeType::Multiply},
                {"divide", ShaderNodeType::Divide}, {"lerp", ShaderNodeType::Lerp},
                {"clamp", ShaderNodeType::Clamp}, {"saturate", ShaderNodeType::Saturate},
                {"one_minus", ShaderNodeType::OneMinus}, {"sin", ShaderNodeType::Sin},
                {"cos", ShaderNodeType::Cos}, {"dot", ShaderNodeType::Dot},
                {"normalize", ShaderNodeType::Normalize}, {"length", ShaderNodeType::Length},
                {"split", ShaderNodeType::Split}, {"combine", ShaderNodeType::Combine},
                {"texture2d", ShaderNodeType::Texture2D}, {"sample_texture2d", ShaderNodeType::SampleTexture2D},
                {"uv", ShaderNodeType::UV}, {"time", ShaderNodeType::Time},
                {"normal", ShaderNodeType::Normal}, {"view_direction", ShaderNodeType::ViewDirection},
                {"fresnel", ShaderNodeType::Fresnel}, {"surface_output", ShaderNodeType::SurfaceOutput},
            };
            for (const auto& [name, type] : nodeTypes) if (id == name) return type;
            return std::nullopt;
        }

        [[nodiscard]] std::optional<ShaderNodeType> shaderNodeTypeFromV1(const int type) {
            // SurfaceOutput was 20 in v1, before Texture2D occupied that value.
            if (type == 20) return ShaderNodeType::SurfaceOutput;
            if (type < static_cast<int>(ShaderNodeType::Float) || type > static_cast<int>(ShaderNodeType::SurfaceOutput)) return std::nullopt;
            return static_cast<ShaderNodeType>(type);
        }

        void writeValue(std::ostream& output, const ShaderNodeValue& value) {
            if (std::holds_alternative<std::monostate>(value)) output << " NONE";
            else if (const auto* number = std::get_if<float>(&value)) output << " FLOAT " << *number;
            else if (const auto* value2 = std::get_if<Vec2>(&value)) output << " VEC2 " << value2->x() << ' ' << value2->y();
            else if (const auto* value3 = std::get_if<Vec3>(&value)) output << " VEC3 " << value3->x() << ' ' << value3->y() << ' ' << value3->z();
            else if (const auto* value4 = std::get_if<Vec4>(&value)) output << " VEC4 " << value4->x() << ' ' << value4->y() << ' ' << value4->z() << ' ' << value4->w();
            else output << " STRING " << std::quoted(std::get<std::string>(value));
        }

        ShaderNodeValue readValue(std::istream& input, const std::filesystem::path& path) {
            std::string tag;
            if (!(input >> tag)) invalid(path);
            if (tag == "NONE") return {};
            float x{};
            float y{};
            float z{};
            float w{};
            if (tag == "FLOAT" && input >> x) return x;
            if (tag == "VEC2" && input >> x >> y) return Vec2{x, y};
            if (tag == "VEC3" && input >> x >> y >> z) return Vec3{x, y, z};
            if (tag == "VEC4" && input >> x >> y >> z >> w) return Vec4{x, y, z, w};
            std::string text;
            if (tag == "STRING" && input >> std::quoted(text)) return text;
            invalid(path);
        }

        void writePin(std::ostream& output, const char* direction, const ShaderPin& pin) {
            output << direction << ' ' << pin.id << ' ' << static_cast<int>(pin.type) << ' ' << std::quoted(pin.name) << '\n';
        }

        ShaderPin readPin(std::istream& input, const std::filesystem::path& path) {
            int type{};
            ShaderPin pin;
            if (!(input >> pin.id >> type >> std::quoted(pin.name))) invalid(path);
            pin.type = static_cast<ShaderValueType>(type);
            return pin;
        }
    } // namespace

    void ShaderGraphSerializer::save(const ShaderGraphAsset& graph, const std::filesystem::path& path) {
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::trunc);
        if (!output) throw std::runtime_error("Could not save shader graph: " + path.string());
        output << "SHADERGRAPH 2\nGRAPH " << graph.id << ' ' << std::quoted(graph.name) << '\n';
        for (const auto& property : graph.properties) {
            output << "PROPERTY " << property.id << ' ' << static_cast<int>(property.type) << ' '
                   << std::quoted(property.name) << ' ' << std::quoted(property.referenceName);
            writeValue(output, property.defaultValue);
            output << '\n';
        }
        for (const auto& node : graph.nodes) {
            output << "NODE " << node.id << ' ' << shaderNodeTypeId(node.type) << ' ' << node.editorPosition.x()
                   << ' ' << node.editorPosition.y() << ' ' << node.propertyId.value_or(NullUUID);
            writeValue(output, node.value);
            output << '\n';
            for (const auto& pin : node.inputs) writePin(output, "INPUT", pin);
            for (const auto& pin : node.outputs) writePin(output, "OUTPUT", pin);
            output << "ENDNODE\n";
        }
        for (const auto& link : graph.links) output << "LINK " << link.id << ' ' << link.fromPin << ' ' << link.toPin << '\n';
        output << "END\n";
    }

    ShaderGraphAsset ShaderGraphSerializer::load(const std::filesystem::path& path) {
        std::ifstream input(path);
        if (!input) throw std::runtime_error("Could not open shader graph: " + path.string());
        std::string tag;
        int version{};
        if (!(input >> tag >> version) || tag != "SHADERGRAPH" || (version != 1 && version != 2)) invalid(path);
        ShaderGraphAsset graph;
        if (!(input >> tag >> graph.id >> std::quoted(graph.name)) || tag != "GRAPH") invalid(path);
        while (input >> tag) {
            if (tag == "END") {
                reserveUUID(graph.id);
                for (const auto& property : graph.properties) reserveUUID(property.id);
                return graph;
            }
            if (tag == "PROPERTY") {
                int type{};
                ShaderProperty property;
                if (!(input >> property.id >> type >> std::quoted(property.name) >> std::quoted(property.referenceName))) invalid(path);
                property.type = static_cast<ShaderValueType>(type);
                property.defaultValue = readValue(input, path);
                graph.properties.push_back(std::move(property));
            } else if (tag == "NODE") {
                std::string typeId;
                UUID propertyId{};
                ShaderNode node;
                float x{};
                float y{};
                if (!(input >> node.id >> typeId >> x >> y >> propertyId)) invalid(path);
                const auto type = version == 1
                                      ? shaderNodeTypeFromV1([&] { try { return std::stoi(typeId); } catch (...) { invalid(path); } }())
                                      : shaderNodeTypeFromId(typeId);
                if (!type) invalid(path);
                node.type = *type;
                node.editorPosition = {x, y};
                node.value = readValue(input, path);
                if (propertyId != NullUUID) node.propertyId = propertyId;
                while (input >> tag && tag != "ENDNODE") {
                    if (tag != "INPUT" && tag != "OUTPUT") invalid(path);
                    ShaderPin pin = readPin(input, path);
                    if (tag == "INPUT") node.inputs.push_back(std::move(pin));
                    else node.outputs.push_back(std::move(pin));
                }
                if (tag != "ENDNODE") invalid(path);
                graph.nodes.push_back(std::move(node));
            } else if (tag == "LINK") {
                ShaderLink link;
                if (!(input >> link.id >> link.fromPin >> link.toPin)) invalid(path);
                graph.links.push_back(link);
            } else invalid(path);
        }
        invalid(path);
    }
} // namespace Engine
