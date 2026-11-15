#pragma once

#include "Engine/Renderer/Materials/Material.h"
#include "Engine/Renderer/ShaderGraph/ShaderGraphVulkan.h"
#include "Engine/Renderer/Vulkan/graphics_pipeline.h"

#include <array>
#include <cstdint>
#include <unordered_set>
#include <vulkan/vulkan.h>

namespace Engine {
namespace Assets {
class AssetManager;
}

namespace Culling {
class IndexedIndirectDrawCount;
}

class ForwardPass final {
  public:
    void create(VkDevice device, VkFormat colorFormat, VkFormat depthFormat, VkSampleCountFlagBits samples,
                VkFormat depthResolveFormat, VkResolveModeFlagBits depthResolveMode, VkDescriptorSetLayout sceneLayout,
                Assets::AssetManager& assets, VkImageLayout colorInitialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                bool colorInitialLayoutExternallySynchronized = false, VkFormat velocityFormat = VK_FORMAT_UNDEFINED,
                VkFormat viewNormalFormat = VK_FORMAT_UNDEFINED, bool preserveDepth = false, bool depthOnly = false,
                bool enableMeshletPipelines = false, VkFormat localLightDataFormat = VK_FORMAT_UNDEFINED,
                VkDescriptorSetLayout meshletDrawLayout = VK_NULL_HANDLE);

    void destroy() noexcept;

    void begin(VkCommandBuffer commandBuffer, VkImageView colorView, VkImageView depthView, VkImageView velocityView,
               VkImageView viewNormalView, VkImageView colorResolveView, VkImageView depthResolveView,
               VkExtent2D extent, VkDescriptorSet sceneDescriptorSet, VkBuffer vertexBuffer, VkBuffer instanceBuffer,
               VkBuffer indexBuffer, VkImageView localLightDataView = VK_NULL_HANDLE) const;

    static void draw(VkCommandBuffer commandBuffer, const Culling::IndexedIndirectDrawCount& indirectDraw);

    void drawMaterial(VkCommandBuffer commandBuffer, VkDescriptorSet sceneDescriptorSet, std::uint32_t shaderSlot,
                      const Culling::IndexedIndirectDrawCount& indirectDraw, VkDeviceSize commandOffset = 0,
                      VkDeviceSize countOffset = 0, bool twoSided = false) const;

    void drawMeshletMaterial(VkCommandBuffer commandBuffer, VkDescriptorSet sceneDescriptorSet,
                             std::uint32_t shaderSlot, const Culling::IndexedIndirectDrawCount& indirectDraw,
                             VkDeviceSize commandOffset, VkDeviceSize countOffset) const;

    /** True when create() received a meshlet draw layout (mesh-shader capable device). */
    [[nodiscard]] bool hasMeshShaderPipelines() const noexcept {
        return drawMeshTasksIndirect_ != nullptr;
    }

    /** True when @p shaderSlot (built-in or Shader Graph) has a mesh-shader pipeline. */
    [[nodiscard]] bool hasMeshShaderPipeline(std::uint32_t shaderSlot) const noexcept;

    /**
     * Draws one material slot's visible meshlets with its mesh-shader
     * pipeline. `meshletDrawSet` is descriptor set 1 of MeshletDraw.slang and
     * `commands` holds one VkDrawMeshTasksIndirectCommandEXT per slot.
     */
    void drawMeshShaderMaterial(VkCommandBuffer commandBuffer, VkDescriptorSet sceneDescriptorSet,
                                VkDescriptorSet meshletDrawSet, std::uint32_t shaderSlot, VkBuffer commands) const;

    /** Adds a cooked graph module to the Vulkan pipeline cache. Render-thread only. */
    [[nodiscard]] std::uint32_t registerShaderGraph(const ShaderGraphProgram& program,
                                                    const MaterialRenderState& state);
    [[nodiscard]] bool hasMaterialPipeline(std::uint32_t shaderSlot) const noexcept;
    void drawShaderGraph(VkCommandBuffer commandBuffer, VkDescriptorSet sceneDescriptorSet, std::uint32_t shaderSlot,
                         const Culling::IndexedIndirectDrawCount& indirectDraw, VkDeviceSize commandOffset = 0,
                         VkDeviceSize countOffset = 0) const;

    void drawFoliage(VkCommandBuffer commandBuffer, VkDescriptorSet sceneDescriptorSet,
                     const Culling::IndexedIndirectDrawCount& indirectDraw) const;
    void drawGrass(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                   const Culling::IndexedIndirectDrawCount& indirectDraw) const;


    static void end(VkCommandBuffer commandBuffer);

    /**
     * Draw with the reduced shader variants, which compile out a feature the
     * frame does not use: displacement in the depth prepass (`_flat`), the
     * DDGI probe lookup in the lighting pass (`_noddgi`). Programs without
     * such a variant keep their regular pipeline.
     */
    void useReducedShaders(bool reduced) noexcept {
        useReduced_ = reduced;
    }

    /**
     * Lighting pass: draw mesh-shader programs with their `_flat` variants,
     * which drop the displacement interpolants. Valid only while no material
     * is displaced. The depth prepass covers this with useReducedShaders.
     */
    void useFlatMeshShaders(bool flat) noexcept {
        useFlatMeshShaders_ = flat;
    }

    /**
     * Lighting pass: draw the resolved PBR mesh-shader programs with their
     * `_deferred` variants, which compile out the local light walk. Valid only
     * while DeferredLocalLightingPass adds the local lights and no PBR debug
     * view is shown (the variants always write the deferred surface inputs).
     */
    void useDeferredLocalShaders(bool deferred) noexcept {
        useDeferredLocal_ = deferred;
    }

    /**
     * Built-in program slots the visibility-buffer resolve shades, each with
     * its own program (visibilityClassOfSlot in MeshletDraw.slang).
     */
    [[nodiscard]] static constexpr bool isVisibilityResolvedSlot(std::size_t slot) noexcept {
        return slot == materialShaderIndex(MaterialShader::StandardPBR) || slot == PbrNormalProgramSlot ||
               slot == PbrExtendedProgramSlot || slot == PbrFoliageProgramSlot;
    }

    /** True for a depth prepass created with an R32_UINT colour target (visibility ids). */
    [[nodiscard]] bool writesVisibilityIds() const noexcept {
        return visibilityIds_;
    }

  private:
    std::array<GraphicsPipeline, MaterialShaderCount> materialPipelines_;
    std::array<GraphicsPipeline, MaterialShaderCount> twoSidedMaterialPipelines_;
    std::array<GraphicsPipeline, MaterialShaderCount> meshletMaterialPipelines_;
    GraphicsPipeline foliagePipeline_;
    // Twins built from the reduced shader variants (see useReducedShaders).
    std::array<GraphicsPipeline, MaterialShaderCount> reducedMaterialPipelines_;
    std::array<GraphicsPipeline, MaterialShaderCount> reducedTwoSidedMaterialPipelines_;
    std::array<GraphicsPipeline, MaterialShaderCount> reducedMeshletMaterialPipelines_;
    // VK_EXT_mesh_shader twins of the meshlet pipelines (`_meshshader` modules).
    std::array<GraphicsPipeline, MaterialShaderCount> meshShaderMaterialPipelines_;
    std::array<GraphicsPipeline, MaterialShaderCount> reducedMeshShaderMaterialPipelines_;
    // `_flat_meshshader` twins of the two above (see useFlatMeshShaders).
    std::array<GraphicsPipeline, MaterialShaderCount> flatMeshShaderMaterialPipelines_;
    std::array<GraphicsPipeline, MaterialShaderCount> flatReducedMeshShaderMaterialPipelines_;
    bool useFlatMeshShaders_ = false;
    // Depth prepass `_opaque` twins of the regular and reduced mesh-shader
    // pipelines: no alpha test, early depth testing. drawMeshShaderMaterial
    // draws a slot's one-sided meshlets with them and only its two-sided
    // (foliage, masked, blended or double-sided) meshlets with the
    // alpha-tested pipeline.
    std::array<GraphicsPipeline, MaterialShaderCount> opaqueMeshShaderMaterialPipelines_;
    std::array<GraphicsPipeline, MaterialShaderCount> opaqueReducedMeshShaderMaterialPipelines_;
    // `_deferred` twins of the four mesh-shader sets above, indexed by
    // (reduced ? 1 : 0) | (flat ? 2 : 0) (see useDeferredLocalShaders).
    std::array<std::array<GraphicsPipeline, MaterialShaderCount>, 4> deferredMeshShaderMaterialPipelines_;
    bool useDeferredLocal_ = false;
    bool visibilityIds_ = false;
    // MeshletDrawSkipOneSided / MeshletDrawSkipTwoSided (MeshletDraw.slang).
    static constexpr std::uint32_t MeshShaderSkipOneSidedFlag = 8U;
    static constexpr std::uint32_t MeshShaderSkipTwoSidedFlag = 16U;
    // MeshletDrawConstants::flags per slot (MeshletDraw.slang).
    std::array<std::uint32_t, MaterialShaderCount> meshShaderDrawFlags_{};
    std::uint32_t meshShaderSmallPrimitiveFlag_ = 0;
    PFN_vkCmdDrawMeshTasksIndirectEXT drawMeshTasksIndirect_ = nullptr;
    GraphicsPipeline reducedFoliagePipeline_;
    bool useReduced_ = false;
    GraphicsPipeline grassPipeline_;
    ShaderGraphPipelineCache shaderGraphPipelines_;
    GraphicsPipelineOptions shaderGraphPipelineOptions_{};
    bool hasVelocityAttachment_ = false;
    bool hasViewNormalAttachment_ = false;
    // Lighting pass only: shading inputs for DeferredLocalLightingPass.
    bool hasLocalLightDataAttachment_ = false;
    // The lighting pass reads the depth prepass with a biased
    // VK_COMPARE_OP_LESS_OR_EQUAL (see create()).
    // Dynamic rendering must carry the matching load-op/layout explicitly.
    bool preserveDepth_ = false;
    // Avoid flooding the editor console when a stale GPU batch references
    // a missing graph pipeline for multiple frames.
    mutable std::unordered_set<std::uint32_t> reportedMissingShaderGraphSlots_;
};
} // namespace Engine
