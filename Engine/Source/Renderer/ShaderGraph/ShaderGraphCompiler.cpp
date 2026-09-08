#include "Engine/Renderer/ShaderGraph/ShaderGraphCompiler.h"

#include <algorithm>
#include <format>
#include <functional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace Engine {
    namespace {
        struct CompiledValue final {
            ShaderIRValue value;
            std::optional<float> scalarConstant;
        };

        [[nodiscard]] const char *slangType(const ShaderValueType type) {
            switch (type) {
                case ShaderValueType::Float: return "float";
                case ShaderValueType::Float2: return "float2";
                case ShaderValueType::Float3: return "float3";
                case ShaderValueType::Float4: return "float4";
                case ShaderValueType::Bool: return "bool";
                case ShaderValueType::Texture2D: return "Sampler2D";
            }
            return "<invalid>";
        }

        [[nodiscard]] unsigned dimensions(const ShaderValueType type) noexcept {
            switch (type) {
                case ShaderValueType::Float: return 1;
                case ShaderValueType::Float2: return 2;
                case ShaderValueType::Float3: return 3;
                case ShaderValueType::Float4: return 4;
                default: return 0;
            }
        }

        [[nodiscard]] std::string literal(const ShaderNodeValue &value, const ShaderValueType type) {
            const auto scalar = [](const float v) { return std::format("{:.9g}", v); };
            if (const auto *v = std::get_if<float>(&value)) return scalar(*v);
            if (const auto *v = std::get_if<Vec2>(&value)) return std::format(
                "float2({}, {})", scalar(v->x()), scalar(v->y()));
            if (const auto *v = std::get_if<Vec3>(&value)) return std::format(
                "float3({}, {}, {})", scalar(v->x()), scalar(v->y()), scalar(v->z()));
            if (const auto *v = std::get_if<Vec4>(&value)) return std::format(
                "float4({}, {}, {}, {})", scalar(v->x()), scalar(v->y()), scalar(v->z()), scalar(v->w()));
            return std::string{slangType(type)} + "(0.0)";
        }

        [[nodiscard]] bool isArithmetic(const ShaderNodeType type) noexcept {
            return type == ShaderNodeType::Add || type == ShaderNodeType::Subtract ||
                   type == ShaderNodeType::Multiply || type == ShaderNodeType::Divide;
        }
    }

    bool ShaderGraphCompiler::resolveBinaryType(const ShaderValueType left, const ShaderValueType right,
                                                ShaderValueType &result) noexcept {
        const unsigned leftDimensions = dimensions(left);
        const unsigned rightDimensions = dimensions(right);
        if (leftDimensions == 0 || rightDimensions == 0 || (
                leftDimensions != rightDimensions && leftDimensions != 1 && rightDimensions != 1)) return false;
        result = leftDimensions >= rightDimensions ? left : right;
        return true;
    }

    ShaderGraphCompileResult ShaderGraphCompiler::compile(const ShaderGraphAsset &graph) const {
        ShaderGraphCompileResult result;
        std::unordered_map<ShaderNodeId, const ShaderNode *> nodes;
        std::unordered_map<ShaderPinId, const ShaderNode *> pinOwners;
        std::unordered_map<ShaderPinId, const ShaderPin *> pins;
        std::unordered_map<ShaderPinId, ShaderPinId> incoming;
        std::unordered_map<UUID, const ShaderProperty *> properties;
        const ShaderNode *output = nullptr;
        const auto error = [&](std::string message, std::vector<ShaderNodeId> ids = {}) {
            result.diagnostics.push_back({std::move(message), std::move(ids)});
        };

        std::unordered_set<std::string> propertyReferenceNames;
        for (const ShaderProperty &property: graph.properties) {
            if (!properties.emplace(property.id, &property).second || property.referenceName.empty() ||
                !propertyReferenceNames.insert(property.referenceName).second) {
                error("Shader property IDs and reference names must be unique and non-empty.");
            }
        }
        for (const ShaderNode &node: graph.nodes) {
            if (!nodes.emplace(node.id, &node).second) error(std::format("Duplicate shader node {}.", node.id),
                                                             {node.id});
            if (node.type == ShaderNodeType::SurfaceOutput) {
                if (output != nullptr) error("A shader graph must have exactly one SurfaceOutput node.",
                                             {output->id, node.id});
                output = &node;
            }
            for (const ShaderPin &pin: node.inputs) {
                if (!pins.emplace(pin.id, &pin).second) error(std::format("Duplicate shader pin {}.", pin.id),
                                                              {node.id});
                pinOwners.emplace(pin.id, &node);
            }
            for (const ShaderPin &pin: node.outputs) {
                if (!pins.emplace(pin.id, &pin).second) error(std::format("Duplicate shader pin {}.", pin.id),
                                                              {node.id});
                pinOwners.emplace(pin.id, &node);
            }
        }
        if (output == nullptr) error("A shader graph requires one SurfaceOutput node.");
        for (const ShaderLink &link: graph.links) {
            const auto from = pinOwners.find(link.fromPin);
            const auto to = pinOwners.find(link.toPin);
            if (from == pinOwners.end() || to == pinOwners.end()) {
                error("A shader link references a missing pin.");
                continue;
            }
            if (std::ranges::none_of(from->second->outputs,
                                     [&](const ShaderPin &pin) { return pin.id == link.fromPin; }) ||
                std::ranges::none_of(to->second->inputs, [&](const ShaderPin &pin) { return pin.id == link.toPin; })) {
                error("Shader links must connect an output pin to an input pin.");
                continue;
            }
            if (!incoming.emplace(link.toPin, link.fromPin).second) error(
                "An input pin may have only one connection.", {to->second->id});
        }
        if (!result.succeeded()) return result;

        enum class Visit { Unvisited, Visiting, Visited };
        std::unordered_map<ShaderNodeId, Visit> visit;
        std::vector<ShaderNodeId> stack;
        std::function<bool(const ShaderNode *)> detectCycles = [&](const ShaderNode *node) {
            Visit &state = visit[node->id];
            if (state == Visit::Visiting) {
                auto begin = std::find(stack.begin(), stack.end(), node->id);
                std::vector<ShaderNodeId> cycle(begin, stack.end());
                cycle.push_back(node->id);
                error("Shader graph contains a cycle.", std::move(cycle));
                return false;
            }
            if (state == Visit::Visited) return true;
            state = Visit::Visiting;
            stack.push_back(node->id);
            for (const ShaderPin &input: node->inputs)
                if (const auto it = incoming.find(input.id); it != incoming.end()) {
                    if (!detectCycles(pinOwners.at(it->second))) return false;
                }
            stack.pop_back();
            state = Visit::Visited;
            return true;
        };
        if (!detectCycles(output)) return result;

        std::unordered_map<ShaderPinId, CompiledValue> compiled;
        std::uint32_t nextValue = 0;
        const auto emit = [&](const ShaderIROp op, const ShaderValueType type, std::vector<ShaderIRValue> operands,
                              std::string payload) {
            const ShaderIRValue value{nextValue++, type};
            result.ir.instructions.push_back({op, value, std::move(operands), std::move(payload)});
            return value;
        };
        std::function<std::optional<CompiledValue>(ShaderPinId)> compilePin;
        compilePin = [&](const ShaderPinId pinId) -> std::optional<CompiledValue> {
            if (const auto cached = compiled.find(pinId); cached != compiled.end()) return cached->second;
            const auto source = incoming.find(pinId);
            if (source == incoming.end()) {
                error(std::format("Required input pin {} is not connected.", pinId));
                return std::nullopt;
            }
            const ShaderNode *node = pinOwners.at(source->second);
            const ShaderPin *outPin = pins.at(source->second);
            CompiledValue compiledValue{};
            if (node->type == ShaderNodeType::Float || node->type == ShaderNodeType::Vector2 || node->type ==
                ShaderNodeType::Vector3 || node->type == ShaderNodeType::Vector4) {
                compiledValue.value = emit(ShaderIROp::Constant, outPin->type, {}, literal(node->value, outPin->type));
                if (const auto *value = std::get_if<float>(&node->value); outPin->type == ShaderValueType::Float)
                    compiledValue.scalarConstant = *value;
            } else if (node->type == ShaderNodeType::Property) {
                if (!node->propertyId || !properties.contains(*node->propertyId)) {
                    error("Property node references a missing property.", {node->id});
                    return std::nullopt;
                }
                const ShaderProperty *property = properties.at(*node->propertyId);
                if (property->type != outPin->type) {
                    error("Property node output type does not match its property.", {node->id});
                    return std::nullopt;
                }
                compiledValue.value = emit(ShaderIROp::Property, outPin->type, {},
                                           "properties." + property->referenceName);
            } else if (node->type == ShaderNodeType::UV || node->type == ShaderNodeType::Time || node->type ==
                       ShaderNodeType::Normal || node->type == ShaderNodeType::ViewDirection) {
                const char *member = node->type == ShaderNodeType::UV
                                         ? "input.uv0"
                                         : node->type == ShaderNodeType::Time
                                               ? "input.time"
                                               : node->type == ShaderNodeType::Normal
                                                     ? "input.worldNormal"
                                                     : "input.viewDirection";
                compiledValue.value = emit(ShaderIROp::Input, outPin->type, {}, member);
            } else if (isArithmetic(node->type)) {
                if (node->inputs.size() != 2) {
                    error("Binary math nodes require exactly two input pins.", {node->id});
                    return std::nullopt;
                }
                auto left = compilePin(node->inputs[0].id);
                auto right = compilePin(node->inputs[1].id);
                if (!left || !right) return std::nullopt;
                ShaderValueType type{};
                if (!resolveBinaryType(left->value.type, right->value.type, type) || outPin->type != type) {
                    error("Incompatible types on binary math node.", {node->id});
                    return std::nullopt;
                }
                if (left->scalarConstant && right->scalarConstant) {
                    const float a = *left->scalarConstant;
                    const float b = *right->scalarConstant;
                    const float folded = node->type == ShaderNodeType::Add
                                             ? a + b
                                             : node->type == ShaderNodeType::Subtract
                                                   ? a - b
                                                   : node->type == ShaderNodeType::Multiply
                                                         ? a * b
                                                         : a / b;
                    compiledValue.value = emit(ShaderIROp::Constant, type, {}, std::format("{:.9g}", folded));
                    compiledValue.scalarConstant = folded;
                } else {
                    const ShaderIROp op = node->type == ShaderNodeType::Add
                                              ? ShaderIROp::Add
                                              : node->type == ShaderNodeType::Subtract
                                                    ? ShaderIROp::Subtract
                                                    : node->type == ShaderNodeType::Multiply
                                                          ? ShaderIROp::Multiply
                                                          : ShaderIROp::Divide;
                    compiledValue.value = emit(op, type, {left->value, right->value}, {});
                }
            } else {
                error("This shader node is not supported by the MVP compiler.", {node->id});
                return std::nullopt;
            }
            compiled.emplace(pinId, compiledValue);
            return compiledValue;
        };

        struct SurfaceMember {
            const char *pin;
            const char *field;
            ShaderValueType type;
            const char *fallback;
        };
        const SurfaceMember members[] = {
            {"Base Color", "baseColor", ShaderValueType::Float3, "float3(1.0)"},
            {"Metallic", "metallic", ShaderValueType::Float, "0.0"},
            {"Roughness", "roughness", ShaderValueType::Float, "0.55"},
            {"Normal", "normal", ShaderValueType::Float3, "normalize(input.worldNormal)"},
            {"Ambient Occlusion", "ao", ShaderValueType::Float, "1.0"},
            {"Emission", "emission", ShaderValueType::Float3, "float3(0.0)"},
            {"Alpha", "alpha", ShaderValueType::Float, "1.0"},
            {"Alpha Cutoff", "alphaCutoff", ShaderValueType::Float, "0.5"}
        };
        std::ostringstream assignments;
        for (const SurfaceMember &member: members) {
            const auto pin = std::ranges::find_if(output->inputs, [&](const ShaderPin &candidate) {
                return candidate.name == member.pin;
            });
            if (pin == output->inputs.end() || pin->type != member.type) {
                error(std::format("SurfaceOutput requires a '{}' {} pin.", member.pin, slangType(member.type)),
                      {output->id});
                continue;
            }
            if (const auto link = incoming.find(pin->id); link != incoming.end()) {
                auto value = compilePin(pin->id);
                if (!value) continue;
                std::string expression = std::format("v{}", value->value.id);
                if (value->value.type == ShaderValueType::Float && member.type != ShaderValueType::Float)
                    expression = std::format("{}({})", slangType(member.type), expression);
                else if (value->value.type != member.type) {
                    error(std::format("Surface input '{}' has an incompatible type.", member.pin), {output->id});
                    continue;
                }
                assignments << "    result." << member.field << " = " << expression << ";\n";
            } else assignments << "    result." << member.field << " = " << member.fallback << ";\n";
        }
        if (!result.succeeded()) {
            result.ir.instructions.clear();
            return result;
        }

        std::ostringstream slang;
        slang << "MaterialSurface evaluateMaterial(SurfaceInput input)\n{\n";
        for (const ShaderIRInstruction &instruction: result.ir.instructions) {
            slang << "    " << slangType(instruction.result.type) << " v" << instruction.result.id << " = ";
            switch (instruction.operation) {
                case ShaderIROp::Constant:
                case ShaderIROp::Input:
                case ShaderIROp::Property: slang << instruction.payload;
                    break;
                case ShaderIROp::Add: slang << "v" << instruction.operands[0].id << " + v" << instruction.operands[1].
                                      id;
                    break;
                case ShaderIROp::Subtract: slang << "v" << instruction.operands[0].id << " - v" << instruction.operands[
                                               1].id;
                    break;
                case ShaderIROp::Multiply: slang << "v" << instruction.operands[0].id << " * v" << instruction.operands[
                                               1].id;
                    break;
                case ShaderIROp::Divide: slang << "v" << instruction.operands[0].id << " / v" << instruction.operands[1]
                                         .id;
                    break;
            }
            slang << ";\n";
        }
        slang << "    MaterialSurface result;\n" << assignments.str() << "    return result;\n}\n";
        result.slang = slang.str();
        return result;
    }
} // namespace Engine
