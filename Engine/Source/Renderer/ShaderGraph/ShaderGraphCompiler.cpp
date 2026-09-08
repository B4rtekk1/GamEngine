#include "Engine/Renderer/ShaderGraph/ShaderGraphCompiler.h"
#include "Engine/Renderer/ShaderGraph/ShaderNodeRegistry.h"

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
            if (const auto *v = std::get_if<Vec2>(&value))
                return std::format(
                    "float2({}, {})", scalar(v->x()), scalar(v->y()));
            if (const auto *v = std::get_if<Vec3>(&value))
                return std::format(
                    "float3({}, {}, {})", scalar(v->x()), scalar(v->y()), scalar(v->z()));
            if (const auto *v = std::get_if<Vec4>(&value))
                return std::format(
                    "float4({}, {}, {}, {})", scalar(v->x()), scalar(v->y()), scalar(v->z()), scalar(v->w()));
            return std::string{slangType(type)} + "(0.0)";
        }

        [[nodiscard]] ShaderIROp arithmeticOperation(const ShaderNodeType type) {
            switch (type) {
                case ShaderNodeType::Add: return ShaderIROp::Add;
                case ShaderNodeType::Subtract: return ShaderIROp::Subtract;
                case ShaderNodeType::Multiply: return ShaderIROp::Multiply;
                default: return ShaderIROp::Divide;
            }
        }
    }

    bool ShaderGraphCompiler::resolveBinaryType(const ShaderValueType left, const ShaderValueType right,
                                                ShaderValueType &result) noexcept {
        const unsigned leftDimensions = dimensions(left);
        const unsigned rightDimensions = dimensions(right);
        if (leftDimensions == 0 || rightDimensions == 0 || (
                leftDimensions != rightDimensions && leftDimensions != 1 && rightDimensions != 1))
            return false;
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
            if (!nodes.emplace(node.id, &node).second)
                error(std::format("Duplicate shader node {}.", node.id),
                      {node.id});
            if (node.type == ShaderNodeType::SurfaceOutput) {
                if (output != nullptr)
                    error("A shader graph must have exactly one SurfaceOutput node.",
                          {output->id, node.id});
                output = &node;
            }
            for (const ShaderPin &pin: node.inputs) {
                if (!pins.emplace(pin.id, &pin).second)
                    error(std::format("Duplicate shader pin {}.", pin.id),
                          {node.id});
                pinOwners.emplace(pin.id, &node);
            }
            for (const ShaderPin &pin: node.outputs) {
                if (!pins.emplace(pin.id, &pin).second)
                    error(std::format("Duplicate shader pin {}.", pin.id),
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
            if (!incoming.emplace(link.toPin, link.fromPin).second)
                error(
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
            const ShaderNodeDefinition *definition = ShaderNodeRegistry::find(node->type);
            if (definition == nullptr) {
                error("Shader node is not registered.", {node->id});
                return std::nullopt;
            }
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
            } else if (definition->compileKind == ShaderNodeCompileKind::BinaryArithmetic) {
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
                    compiledValue.value = emit(arithmeticOperation(node->type), type, {left->value, right->value}, {});
                }
            } else if (definition->compileKind == ShaderNodeCompileKind::Lerp ||
                       definition->compileKind == ShaderNodeCompileKind::Clamp) {
                if (node->inputs.size() != 3) {
                    error("Lerp and Clamp nodes require exactly three input pins.", {node->id});
                    return std::nullopt;
                }
                auto a = compilePin(node->inputs[0].id);
                auto b = compilePin(node->inputs[1].id);
                auto c = compilePin(node->inputs[2].id);
                if (!a || !b || !c || a->value.type != ShaderValueType::Float || b->value.type != ShaderValueType::Float
                    ||
                    c->value.type != ShaderValueType::Float || outPin->type != ShaderValueType::Float) {
                    error("Lerp and Clamp currently require scalar inputs.", {node->id});
                    return std::nullopt;
                }
                compiledValue.value = emit(
                    definition->compileKind == ShaderNodeCompileKind::Lerp ? ShaderIROp::Lerp : ShaderIROp::Clamp,
                    ShaderValueType::Float, {a->value, b->value, c->value}, {});
            } else if (definition->compileKind == ShaderNodeCompileKind::Unary) {
                if (node->inputs.size() != 1) {
                    error("Unary node requires exactly one input pin.", {node->id});
                    return std::nullopt;
                }
                auto value = compilePin(node->inputs[0].id);
                if (!value || (node->type != ShaderNodeType::Length && value->value.type != outPin->type)) {
                    error("Unary node input type does not match its output.", {node->id});
                    return std::nullopt;
                }
                const ShaderIROp op = node->type == ShaderNodeType::Saturate
                                          ? ShaderIROp::Saturate
                                          : node->type == ShaderNodeType::OneMinus
                                                ? ShaderIROp::OneMinus
                                                : node->type == ShaderNodeType::Sin
                                                      ? ShaderIROp::Sin
                                                      : node->type == ShaderNodeType::Cos
                                                            ? ShaderIROp::Cos
                                                            : node->type == ShaderNodeType::Normalize
                                                                  ? ShaderIROp::Normalize
                                                                  : ShaderIROp::Length;
                if (op == ShaderIROp::Length && (
                        value->value.type != ShaderValueType::Float3 || outPin->type != ShaderValueType::Float)) {
                    error("Length requires a Float3 input and Float output.", {node->id});
                    return std::nullopt;
                }
                compiledValue.value = emit(op, outPin->type, {value->value}, {});
            } else if (definition->compileKind == ShaderNodeCompileKind::Dot) {
                auto a = compilePin(node->inputs[0].id);
                auto b = compilePin(node->inputs[1].id);
                if (!a || !b || a->value.type != ShaderValueType::Float3 || b->value.type != ShaderValueType::Float3 ||
                    outPin->type != ShaderValueType::Float) {
                    error("Dot requires two Float3 inputs.", {node->id});
                    return std::nullopt;
                }
                compiledValue.value = emit(ShaderIROp::Dot, ShaderValueType::Float, {a->value, b->value}, {});
            } else if (definition->compileKind == ShaderNodeCompileKind::Split) {
                auto value = compilePin(node->inputs[0].id);
                if (!value || value->value.type != ShaderValueType::Float3 || outPin->type != ShaderValueType::Float) {
                    error("Split requires a Float3 input.", {node->id});
                    return std::nullopt;
                }
                const char component = outPin->name == "X" ? 'x' : outPin->name == "Y" ? 'y' : 'z';
                compiledValue.value = emit(ShaderIROp::Split, ShaderValueType::Float, {value->value},
                                           std::string(1, component));
            } else if (definition->compileKind == ShaderNodeCompileKind::Combine) {
                std::vector<ShaderIRValue> components;
                for (const auto &input: node->inputs) {
                    auto value = compilePin(input.id);
                    if (!value || value->value.type != ShaderValueType::Float) {
                        error("Combine requires scalar inputs.", {node->id});
                        return std::nullopt;
                    }
                    components.push_back(value->value);
                }
                compiledValue.value = emit(ShaderIROp::Combine, ShaderValueType::Float3, std::move(components), {});
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
                case ShaderIROp::Lerp: slang << "lerp(v" << instruction.operands[0].id << ", v" << instruction.operands[
                                           1].id << ", v" << instruction.operands[2].id << ')';
                    break;
                case ShaderIROp::Clamp: slang << "clamp(v" << instruction.operands[0].id << ", v" << instruction.
                                        operands[1].id << ", v" << instruction.operands[2].id << ')';
                    break;
                case ShaderIROp::Saturate: slang << "saturate(v" << instruction.operands[0].id << ')';
                    break;
                case ShaderIROp::OneMinus: slang << "1.0 - v" << instruction.operands[0].id;
                    break;
                case ShaderIROp::Sin: slang << "sin(v" << instruction.operands[0].id << ')';
                    break;
                case ShaderIROp::Cos: slang << "cos(v" << instruction.operands[0].id << ')';
                    break;
                case ShaderIROp::Dot: slang << "dot(v" << instruction.operands[0].id << ", v" << instruction.operands[1]
                                      .id << ')';
                    break;
                case ShaderIROp::Normalize: slang << "normalize(v" << instruction.operands[0].id << ')';
                    break;
                case ShaderIROp::Length: slang << "length(v" << instruction.operands[0].id << ')';
                    break;
                case ShaderIROp::Split: slang << "v" << instruction.operands[0].id << '.' << instruction.payload;
                    break;
                case ShaderIROp::Combine: slang << "float3(v" << instruction.operands[0].id << ", v" << instruction.
                                          operands[1].id << ", v" << instruction.operands[2].id << ')';
                    break;
            }
            slang << ";\n";
        }
        slang << "    MaterialSurface result;\n" << assignments.str() << "    return result;\n}\n";
        result.slang = slang.str();
        return result;
    }
} // namespace Engine
