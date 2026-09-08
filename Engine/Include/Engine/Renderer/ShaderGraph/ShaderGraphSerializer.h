#pragma once

#include "Engine/Renderer/ShaderGraph/ShaderGraph.h"

#include <filesystem>

namespace Engine {
    /** Versioned, human-readable persistence for .shadergraph assets. */
    class ShaderGraphSerializer final {
    public:
        static void save(const ShaderGraphAsset& graph, const std::filesystem::path& path);
        [[nodiscard]] static ShaderGraphAsset load(const std::filesystem::path& path);
    };
} // namespace Engine
