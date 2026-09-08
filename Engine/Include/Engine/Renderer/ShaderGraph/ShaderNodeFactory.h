#pragma once

#include "Engine/Renderer/ShaderGraph/ShaderGraph.h"

namespace Engine {
    /** Creates graph nodes with the pin names and types expected by the compiler. */
    class ShaderNodeFactory final {
    public:
        [[nodiscard]] static ShaderNode create(ShaderNodeType type, ShaderNodeId nodeId,
                                               ShaderPinId& nextPinId);
    };
} // namespace Engine
