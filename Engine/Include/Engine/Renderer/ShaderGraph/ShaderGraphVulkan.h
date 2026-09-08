#pragma once

#include "Engine/Renderer/Materials/Material.h"
#include "Engine/Renderer/ShaderGraph/ShaderGraphCompiler.h"
#include "Engine/Renderer/Vulkan/graphics_pipeline.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Engine {
    /** Output of the graph cooker, ready for a Vulkan graphics pipeline. */
    struct ShaderGraphProgram final {
        ShaderProgramId id{};
        std::filesystem::path slangPath;
        std::filesystem::path spirvPath;
    };

    /**
     * Turns a graph into a complete forward shader module. The graph controls
     * only the surface contract; vertex transform, lighting and shadows remain
     * in Forward/forward_pbr.slang.
     */
    class ShaderGraphSlangCompiler final {
    public:
        [[nodiscard]] ShaderGraphCompileResult compile(const ShaderGraphAsset& graph,
                                                        const std::filesystem::path& forwardTemplate,
                                                        const std::filesystem::path& generatedDirectory,
                                                        ShaderGraphProgram& program) const;

    private:
        [[nodiscard]] static ShaderProgramId makeProgramId(std::string_view generatedSurface) noexcept;
    };

    /** Resolves the authoring .shadergraph reference on a material to cooked runtime data. */
    class ShaderGraphMaterialCompiler final {
    public:
        /**
         * Compiles material.shaderGraphAsset relative to assetRoot.  On failure
         * the material is unchanged, so a visible renderer never receives a
         * half-cooked program.
         */
        [[nodiscard]] ShaderGraphCompileResult resolve(Material& material,
                                                        const std::filesystem::path& assetRoot,
                                                        const std::filesystem::path& forwardTemplate,
                                                        const std::filesystem::path& generatedDirectory) const;
    };

    /** Render-thread owned cache. One graph program creates one VkPipeline per render state. */
    class ShaderGraphPipelineCache final {
    public:
        void initialize(VkDevice device, GraphicsPipelineOptions baseOptions);
        void destroy() noexcept;

        [[nodiscard]] std::uint32_t getOrCreate(ShaderProgramId program,
                                                const std::filesystem::path& spirv,
                                                const MaterialRenderState& state);
        [[nodiscard]] const GraphicsPipeline* find(std::uint32_t slot) const noexcept;

    private:
        struct Entry final {
            std::filesystem::path spirv;
            MaterialRenderState state{};
            std::uint32_t slot{};
            std::unique_ptr<GraphicsPipeline> pipeline;
        };

        VkDevice device_{VK_NULL_HANDLE};
        GraphicsPipelineOptions baseOptions_{};
        std::unordered_map<ShaderProgramId, Entry> entries_;
    };
} // namespace Engine
