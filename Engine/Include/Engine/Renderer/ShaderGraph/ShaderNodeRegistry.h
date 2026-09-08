#pragma once

#include "Engine/Renderer/ShaderGraph/ShaderGraph.h"

#include <functional>
#include <span>
#include <string_view>

namespace Engine {
    /** Describes the compiler path used by a registered node. */
    enum class ShaderNodeCompileKind : std::uint8_t {
        Constant, Property, Input, BinaryArithmetic, Lerp, Clamp, Unary, Dot, Split, Combine, SurfaceOutput,
        Unsupported
    };

    /** Stable node metadata shared by the editor, node factory and compiler. */
    struct ShaderNodeDefinition final {
        ShaderNodeType type;
        std::string_view displayName;
        std::string_view category;
        /** Optional second-level editor menu, e.g. Math / Trigonometry. */
        std::string_view subcategory;
        ShaderNodeCompileKind compileKind{ShaderNodeCompileKind::Unsupported};
        /** Creates all pins and assigns their graph-local ids. */
        std::function<void(ShaderNode&, ShaderPinId&)> createPins;
    };

    class ShaderNodeRegistry final {
    public:
        [[nodiscard]] static const ShaderNodeDefinition* find(ShaderNodeType type) noexcept;
        [[nodiscard]] static std::span<const ShaderNodeDefinition> definitions() noexcept;
        [[nodiscard]] static ShaderNode create(ShaderNodeType type, ShaderNodeId nodeId, ShaderPinId& nextPinId);
    };
} // namespace Engine
