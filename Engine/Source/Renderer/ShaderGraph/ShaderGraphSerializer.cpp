#include "Engine/Renderer/ShaderGraph/ShaderGraphSerializer.h"

#include <fstream>
#include <iomanip>
#include <stdexcept>

namespace Engine {
    namespace {
        [[noreturn]] void invalid(const std::filesystem::path& path) {
            throw std::runtime_error("Invalid shader graph: " + path.string());
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
        output << "SHADERGRAPH 1\nGRAPH " << graph.id << ' ' << std::quoted(graph.name) << '\n';
        for (const auto& property : graph.properties) {
            output << "PROPERTY " << property.id << ' ' << static_cast<int>(property.type) << ' '
                   << std::quoted(property.name) << ' ' << std::quoted(property.referenceName);
            writeValue(output, property.defaultValue);
            output << '\n';
        }
        for (const auto& node : graph.nodes) {
            output << "NODE " << node.id << ' ' << static_cast<int>(node.type) << ' ' << node.editorPosition.x()
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
        if (!(input >> tag >> version) || tag != "SHADERGRAPH" || version != 1) invalid(path);
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
                int type{};
                UUID propertyId{};
                ShaderNode node;
                float x{};
                float y{};
                if (!(input >> node.id >> type >> x >> y >> propertyId)) invalid(path);
                node.type = static_cast<ShaderNodeType>(type);
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
