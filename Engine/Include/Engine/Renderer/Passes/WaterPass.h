#pragma once

#include "Engine/Renderer/Vulkan/graphics_pipeline.h"

#include <vulkan/vulkan.h>

namespace Engine {
namespace Culling { class IndexedIndirectDrawCount; }

/**
 * Dedicated compositing pass for water surfaces.
 *
 * It loads opaque HDR/depth, samples a copied opaque scene in the shader and
 * writes only the displaced water surface.  Keeping this out of ForwardPass
 * prevents water from becoming an opaque depth-prepass material.
 */
class WaterPass final {
public:
    void create(VkDevice device, VkFormat colorFormat, VkFormat depthFormat,
                VkSampleCountFlagBits samples, VkFormat depthResolveFormat,
                VkResolveModeFlagBits depthResolveMode, VkDescriptorSetLayout sceneLayout,
                Assets::AssetManager& assets, bool velocity);
    void destroy() noexcept;
    void begin(VkCommandBuffer commandBuffer, VkFramebuffer framebuffer, VkExtent2D extent,
               VkDescriptorSet descriptorSet, VkBuffer vertexBuffer, VkBuffer indexBuffer) const;
    void draw(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
              const Culling::IndexedIndirectDrawCount& indirectDraw,
              VkDeviceSize commandOffset, VkDeviceSize countOffset) const;
    static void end(VkCommandBuffer commandBuffer);
    [[nodiscard]] VkRenderPass renderPass() const noexcept { return pipeline_.renderPass(); }

private:
    GraphicsPipeline pipeline_;
    bool hasVelocity_{};
};
} // namespace Engine
