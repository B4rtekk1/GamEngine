#pragma once

#include "Engine/Renderer/ShaderGraph/ShaderGraph.h"

#include <string>
#include <vector>

namespace Engine {
    enum class ShaderIROp : std::uint8_t {
        Constant, Add, Subtract, Multiply, Divide, Input, Property,
        Lerp, Clamp, Saturate, OneMinus, Sin, Cos, Dot, Normalize, Length, Split, Combine
    };

    struct ShaderIRValue final {
        std::uint32_t id{};
        ShaderValueType type{ShaderValueType::Float};
    };

    struct ShaderIRInstruction final {
        ShaderIROp operation{ShaderIROp::Constant};
        ShaderIRValue result{};
        std::vector<ShaderIRValue> operands;
        /** Literal value, input member, or property reference. */
        std::string payload;
    };

    struct ShaderGraphIR final {
        std::vector<ShaderIRInstruction> instructions;
    };
} // namespace Engine
