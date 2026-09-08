#include "Engine/Renderer/ShaderGraph/ShaderNodeRegistry.h"

#include <array>

namespace Engine {
    namespace {
        ShaderPin input(ShaderPinId& id, const char* name, ShaderValueType type) { return {.id = id++, .name = name, .type = type}; }
        ShaderPin output(ShaderPinId& id, const char* name, ShaderValueType type) { return {.id = id++, .name = name, .type = type}; }
        void noPins(ShaderNode&, ShaderPinId&) {}
        void scalarConstant(ShaderNode& n, ShaderPinId& id) { n.outputs = {output(id, "Value", ShaderValueType::Float)}; n.value = 0.0F; }
        void vector2Constant(ShaderNode& n, ShaderPinId& id) { n.outputs = {output(id, "Value", ShaderValueType::Float2)}; n.value = Vec2{}; }
        void vector3Constant(ShaderNode& n, ShaderPinId& id) { n.outputs = {output(id, "Value", ShaderValueType::Float3)}; n.value = Vec3{}; }
        void vector4Constant(ShaderNode& n, ShaderPinId& id) { n.outputs = {output(id, "Value", ShaderValueType::Float4)}; n.value = Vec4{}; }
        void binary(ShaderNode& n, ShaderPinId& id) { n.inputs = {input(id, "A", ShaderValueType::Float), input(id, "B", ShaderValueType::Float)}; n.outputs = {output(id, "Result", ShaderValueType::Float)}; }
        void lerp(ShaderNode& n, ShaderPinId& id) { n.inputs = {input(id, "A", ShaderValueType::Float), input(id, "B", ShaderValueType::Float), input(id, "T", ShaderValueType::Float)}; n.outputs = {output(id, "Result", ShaderValueType::Float)}; }
        void clamp(ShaderNode& n, ShaderPinId& id) { n.inputs = {input(id, "Value", ShaderValueType::Float), input(id, "Min", ShaderValueType::Float), input(id, "Max", ShaderValueType::Float)}; n.outputs = {output(id, "Result", ShaderValueType::Float)}; }
        void unaryScalar(ShaderNode& n, ShaderPinId& id) { n.inputs = {input(id, "Value", ShaderValueType::Float)}; n.outputs = {output(id, "Result", ShaderValueType::Float)}; }
        void vectorUnary(ShaderNode& n, ShaderPinId& id) { n.inputs = {input(id, "Vector", ShaderValueType::Float3)}; n.outputs = {output(id, "Result", ShaderValueType::Float3)}; }
        void dot(ShaderNode& n, ShaderPinId& id) { n.inputs = {input(id, "A", ShaderValueType::Float3), input(id, "B", ShaderValueType::Float3)}; n.outputs = {output(id, "Result", ShaderValueType::Float)}; }
        void length(ShaderNode& n, ShaderPinId& id) { n.inputs = {input(id, "Vector", ShaderValueType::Float3)}; n.outputs = {output(id, "Length", ShaderValueType::Float)}; }
        void split(ShaderNode& n, ShaderPinId& id) { n.inputs = {input(id, "Vector", ShaderValueType::Float3)}; n.outputs = {output(id, "X", ShaderValueType::Float), output(id, "Y", ShaderValueType::Float), output(id, "Z", ShaderValueType::Float)}; }
        void combine(ShaderNode& n, ShaderPinId& id) { n.inputs = {input(id, "X", ShaderValueType::Float), input(id, "Y", ShaderValueType::Float), input(id, "Z", ShaderValueType::Float)}; n.outputs = {output(id, "Vector", ShaderValueType::Float3)}; }
        void inputUv(ShaderNode& n, ShaderPinId& id) { n.outputs = {output(id, "UV", ShaderValueType::Float2)}; }
        void inputTime(ShaderNode& n, ShaderPinId& id) { n.outputs = {output(id, "Time", ShaderValueType::Float)}; }
        void inputVector(ShaderNode& n, ShaderPinId& id) { n.outputs = {output(id, "Value", ShaderValueType::Float3)}; }
        void surface(ShaderNode& n, ShaderPinId& id) { n.inputs = {input(id, "Base Color", ShaderValueType::Float3), input(id, "Metallic", ShaderValueType::Float), input(id, "Roughness", ShaderValueType::Float), input(id, "Normal", ShaderValueType::Float3), input(id, "Ambient Occlusion", ShaderValueType::Float), input(id, "Emission", ShaderValueType::Float3), input(id, "Alpha", ShaderValueType::Float), input(id, "Alpha Cutoff", ShaderValueType::Float)}; }
        const std::array nodeDefinitions = {
            ShaderNodeDefinition{ShaderNodeType::Float, "Float", "Input", {}, ShaderNodeCompileKind::Constant, scalarConstant}, ShaderNodeDefinition{ShaderNodeType::Vector2, "Vector 2", "Input", {}, ShaderNodeCompileKind::Constant, vector2Constant}, ShaderNodeDefinition{ShaderNodeType::Vector3, "Vector 3", "Input", {}, ShaderNodeCompileKind::Constant, vector3Constant}, ShaderNodeDefinition{ShaderNodeType::Vector4, "Vector 4", "Input", {}, ShaderNodeCompileKind::Constant, vector4Constant},
            ShaderNodeDefinition{ShaderNodeType::Property, "Property", "Input", {}, ShaderNodeCompileKind::Property, noPins},
            ShaderNodeDefinition{ShaderNodeType::Add, "Add", "Math", "Basic", ShaderNodeCompileKind::BinaryArithmetic, binary}, ShaderNodeDefinition{ShaderNodeType::Subtract, "Subtract", "Math", "Basic", ShaderNodeCompileKind::BinaryArithmetic, binary}, ShaderNodeDefinition{ShaderNodeType::Multiply, "Multiply", "Math", "Basic", ShaderNodeCompileKind::BinaryArithmetic, binary}, ShaderNodeDefinition{ShaderNodeType::Divide, "Divide", "Math", "Basic", ShaderNodeCompileKind::BinaryArithmetic, binary},
            ShaderNodeDefinition{ShaderNodeType::Lerp, "Lerp", "Math", "Basic", ShaderNodeCompileKind::Lerp, lerp}, ShaderNodeDefinition{ShaderNodeType::Clamp, "Clamp", "Math", "Basic", ShaderNodeCompileKind::Clamp, clamp}, ShaderNodeDefinition{ShaderNodeType::Saturate, "Saturate", "Math", "Basic", ShaderNodeCompileKind::Unary, unaryScalar}, ShaderNodeDefinition{ShaderNodeType::OneMinus, "One Minus", "Math", "Basic", ShaderNodeCompileKind::Unary, unaryScalar}, ShaderNodeDefinition{ShaderNodeType::Sin, "Sin", "Math", "Trigonometry", ShaderNodeCompileKind::Unary, unaryScalar}, ShaderNodeDefinition{ShaderNodeType::Cos, "Cos", "Math", "Trigonometry", ShaderNodeCompileKind::Unary, unaryScalar},
            ShaderNodeDefinition{ShaderNodeType::Dot, "Dot", "Math", "Vector", ShaderNodeCompileKind::Dot, dot}, ShaderNodeDefinition{ShaderNodeType::Normalize, "Normalize", "Math", "Vector", ShaderNodeCompileKind::Unary, vectorUnary}, ShaderNodeDefinition{ShaderNodeType::Length, "Length", "Math", "Vector", ShaderNodeCompileKind::Unary, length}, ShaderNodeDefinition{ShaderNodeType::Split, "Split", "Utility", {}, ShaderNodeCompileKind::Split, split}, ShaderNodeDefinition{ShaderNodeType::Combine, "Combine", "Utility", {}, ShaderNodeCompileKind::Combine, combine},
            ShaderNodeDefinition{ShaderNodeType::UV, "UV", "Input", {}, ShaderNodeCompileKind::Input, inputUv}, ShaderNodeDefinition{ShaderNodeType::Time, "Time", "Input", {}, ShaderNodeCompileKind::Input, inputTime}, ShaderNodeDefinition{ShaderNodeType::Normal, "Normal", "Input", {}, ShaderNodeCompileKind::Input, inputVector}, ShaderNodeDefinition{ShaderNodeType::ViewDirection, "View Direction", "Input", {}, ShaderNodeCompileKind::Input, inputVector},
            ShaderNodeDefinition{ShaderNodeType::Texture2D, "Texture 2D", "Texture", {}, ShaderNodeCompileKind::Unsupported, noPins}, ShaderNodeDefinition{ShaderNodeType::SampleTexture2D, "Sample Texture 2D", "Texture", {}, ShaderNodeCompileKind::Unsupported, noPins}, ShaderNodeDefinition{ShaderNodeType::Fresnel, "Fresnel", "Math", "Vector", ShaderNodeCompileKind::Unsupported, noPins}, ShaderNodeDefinition{ShaderNodeType::SurfaceOutput, "PBR Surface", "Output", {}, ShaderNodeCompileKind::SurfaceOutput, surface}
        };
    }
    const ShaderNodeDefinition* ShaderNodeRegistry::find(ShaderNodeType type) noexcept { for (const auto& definition : nodeDefinitions) if (definition.type == type) return &definition; return nullptr; }
    std::span<const ShaderNodeDefinition> ShaderNodeRegistry::definitions() noexcept { return nodeDefinitions; }
    ShaderNode ShaderNodeRegistry::create(ShaderNodeType type, ShaderNodeId nodeId, ShaderPinId& nextPinId) { ShaderNode node{.id = nodeId, .type = type}; if (const auto* definition = find(type)) definition->createPins(node, nextPinId); return node; }
} // namespace Engine
