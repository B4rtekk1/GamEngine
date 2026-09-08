#include "Engine/Renderer/ShaderGraph/ShaderNodeFactory.h"

namespace Engine {
    namespace {
        ShaderPin input(ShaderPinId& nextId, const char* name, const ShaderValueType type) {
            return {.id = nextId++, .name = name, .type = type};
        }

        ShaderPin output(ShaderPinId& nextId, const char* name, const ShaderValueType type) {
            return {.id = nextId++, .name = name, .type = type};
        }
    }

    ShaderNode ShaderNodeFactory::create(const ShaderNodeType type, const ShaderNodeId nodeId,
                                         ShaderPinId& nextPinId) {
        ShaderNode node{.id = nodeId, .type = type};
        switch (type) {
            case ShaderNodeType::Float:
                node.outputs = {output(nextPinId, "Value", ShaderValueType::Float)};
                node.value = 0.0F;
                break;
            case ShaderNodeType::Vector2:
                node.outputs = {output(nextPinId, "Value", ShaderValueType::Float2)};
                node.value = Vec2{};
                break;
            case ShaderNodeType::Vector3:
                node.outputs = {output(nextPinId, "Value", ShaderValueType::Float3)};
                node.value = Vec3{};
                break;
            case ShaderNodeType::Vector4:
                node.outputs = {output(nextPinId, "Value", ShaderValueType::Float4)};
                node.value = Vec4{};
                break;
            case ShaderNodeType::Add:
            case ShaderNodeType::Subtract:
            case ShaderNodeType::Multiply:
            case ShaderNodeType::Divide:
                node.inputs = {input(nextPinId, "A", ShaderValueType::Float),
                               input(nextPinId, "B", ShaderValueType::Float)};
                node.outputs = {output(nextPinId, "Result", ShaderValueType::Float)};
                break;
            case ShaderNodeType::UV:
                node.outputs = {output(nextPinId, "UV", ShaderValueType::Float2)};
                break;
            case ShaderNodeType::Time:
                node.outputs = {output(nextPinId, "Time", ShaderValueType::Float)};
                break;
            case ShaderNodeType::Normal:
            case ShaderNodeType::ViewDirection:
                node.outputs = {output(nextPinId, "Value", ShaderValueType::Float3)};
                break;
            case ShaderNodeType::SurfaceOutput:
                node.inputs = {
                    input(nextPinId, "Base Color", ShaderValueType::Float3),
                    input(nextPinId, "Metallic", ShaderValueType::Float),
                    input(nextPinId, "Roughness", ShaderValueType::Float),
                    input(nextPinId, "Normal", ShaderValueType::Float3),
                    input(nextPinId, "Ambient Occlusion", ShaderValueType::Float),
                    input(nextPinId, "Emission", ShaderValueType::Float3),
                    input(nextPinId, "Alpha", ShaderValueType::Float),
                    input(nextPinId, "Alpha Cutoff", ShaderValueType::Float)};
                break;
            default:
                // The remaining enum members are reserved for the compiler's next node-set expansion.
                break;
        }
        return node;
    }
} // namespace Engine
