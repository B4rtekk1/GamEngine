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
    /// Color-only module used by passes without a velocity attachment.
    std::filesystem::path noVelocitySpirvPath;
    /// VK_EXT_mesh_shader twins of the two modules above.
    std::filesystem::path meshSpirvPath;
    std::filesystem::path meshNoVelocitySpirvPath;
};

/** Mesh-shader twin of a cooked graph module: `<stem>_meshshader.spv` beside it. */
[[nodiscard]] std::filesystem::path meshShaderGraphModule(const std::filesystem::path& module);

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
    [[nodiscard]] ShaderGraphCompileResult resolve(Material& material, const std::filesystem::path& assetRoot,
                                                   const std::filesystem::path& forwardTemplate,
                                                   const std::filesystem::path& generatedDirectory) const;
};

/** Render-thread owned cache. One graph program creates one VkPipeline per render state. */
class ShaderGraphPipelineCache final {
  public:
    /// @p meshletDrawLayout: set 1 of the mesh-shader twins; null builds none.
    void initialize(VkDevice device, GraphicsPipelineOptions baseOptions,
                    VkDescriptorSetLayout meshletDrawLayout = VK_NULL_HANDLE);
    // Releases Vulkan pipelines but preserves program-to-slot mappings
    // across render-pass recreation.
    void destroy() noexcept;

    [[nodiscard]] std::uint32_t getOrCreate(ShaderProgramId program, const std::filesystem::path& spirv,
                                            const MaterialRenderState& state);
    [[nodiscard]] const GraphicsPipeline* find(std::uint32_t slot) const noexcept;
    /// Mesh-shader twin of the slot's pipeline, or null when it has none.
    [[nodiscard]] const GraphicsPipeline* findMesh(std::uint32_t slot, bool* doubleSided = nullptr) const noexcept;

  private:
    struct Entry final {
        std::filesystem::path spirv;
        MaterialRenderState state{};
        std::uint32_t slot{};
        std::unique_ptr<GraphicsPipeline> pipeline;
        std::unique_ptr<GraphicsPipeline> meshPipeline;
    };

    void createPipeline(Entry& entry);

    VkDevice device_{VK_NULL_HANDLE};
    GraphicsPipelineOptions baseOptions_{};
    VkDescriptorSetLayout meshletDrawLayout_{VK_NULL_HANDLE};
    std::unordered_map<ShaderProgramId, Entry> entries_;
};
} // namespace Engine
