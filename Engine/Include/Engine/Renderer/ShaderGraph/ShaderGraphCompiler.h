#pragma once

#include "Engine/Renderer/ShaderGraph/ShaderGraphIR.h"

#include <string>
#include <vector>

namespace Engine {
    struct ShaderGraphDiagnostic final {
        std::string message;
        std::vector<ShaderNodeId> nodes;
    };

    struct ShaderGraphCompileResult final {
        ShaderGraphIR ir;
        std::string slang;
        std::vector<ShaderGraphDiagnostic> diagnostics;

        [[nodiscard]] bool succeeded() const noexcept { return diagnostics.empty(); }
    };

    /** Compiles only the dependencies reachable from the single SurfaceOutput node. */
    class ShaderGraphCompiler final {
    public:
        [[nodiscard]] ShaderGraphCompileResult compile(const ShaderGraphAsset& graph) const;

        /** Binary arithmetic permits scalar splatting, but never implicit vector truncation. */
        [[nodiscard]] static bool resolveBinaryType(ShaderValueType left, ShaderValueType right,
                                                    ShaderValueType& result) noexcept;
    };
} // namespace Engine
