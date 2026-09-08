#pragma once

#include "Engine/Renderer/Materials/Material.h"
#include "Engine/Renderer/ShaderGraph/ShaderGraphVulkan.h"
#include "Engine/Renderer/Vulkan/graphics_pipeline.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>

namespace Engine {
    namespace Assets {
        class AssetManager;
    }

    namespace Culling {
        class IndexedIndirectDrawCount;
    }

    class ForwardPass final {
    public:
        void create(VkDevice device, VkFormat colorFormat, VkFormat depthFormat,
                    VkSampleCountFlagBits samples,
                    VkFormat depthResolveFormat, VkResolveModeFlagBits depthResolveMode,
                    VkDescriptorSetLayout sceneLayout,
                    Assets::AssetManager &assets);

        void destroy() noexcept;

        void begin(VkCommandBuffer commandBuffer, VkFramebuffer framebuffer,
                   VkExtent2D extent, VkDescriptorSet sceneDescriptorSet,
                   VkBuffer vertexBuffer, VkBuffer instanceBuffer,
                   VkBuffer indexBuffer) const;

        static void draw(VkCommandBuffer commandBuffer,
                         const Culling::IndexedIndirectDrawCount &indirectDraw);

        void drawMaterial(VkCommandBuffer commandBuffer, VkDescriptorSet sceneDescriptorSet,
                          std::uint32_t shaderSlot,
                          const Culling::IndexedIndirectDrawCount& indirectDraw,
                          VkDeviceSize commandOffset = 0,
                          VkDeviceSize countOffset = 0) const;

        /** Adds a cooked graph module to the Vulkan pipeline cache. Render-thread only. */
        [[nodiscard]] std::uint32_t registerShaderGraph(const ShaderGraphProgram& program,
                                                        const MaterialRenderState& state);
        void drawShaderGraph(VkCommandBuffer commandBuffer, VkDescriptorSet sceneDescriptorSet,
                             std::uint32_t shaderSlot, const Culling::IndexedIndirectDrawCount& indirectDraw,
                             VkDeviceSize commandOffset = 0, VkDeviceSize countOffset = 0) const;

        void drawFoliage(VkCommandBuffer commandBuffer,
                         VkDescriptorSet sceneDescriptorSet,
                         const Culling::IndexedIndirectDrawCount &indirectDraw) const;
        void drawGrass(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
                       const Culling::IndexedIndirectDrawCount& indirectDraw) const;

        void drawOutline(VkCommandBuffer commandBuffer,
                         VkDescriptorSet sceneDescriptorSet,
                         const Culling::IndexedIndirectDrawCount &indirectDraw) const;

        static void end(VkCommandBuffer commandBuffer);

        [[nodiscard]] VkRenderPass renderPass() const noexcept {
            return materialPipelines_[0].renderPass();
        }

    private:
        std::array<GraphicsPipeline, MaterialShaderCount> materialPipelines_;
        GraphicsPipeline foliagePipeline_;
        GraphicsPipeline grassPipeline_;
        GraphicsPipeline outlinePipeline_;
        ShaderGraphPipelineCache shaderGraphPipelines_;
        GraphicsPipelineOptions shaderGraphPipelineOptions_{};
    };
} // namespace Engine
