#pragma once

#include "Engine/Renderer/Vulkan/graphics_pipeline.h"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace Engine {
namespace Assets { class AssetManager; }

namespace Culling {
class IndexedIndirectDrawCount;
}

class ForwardPass final {
public:
    void create(VkDevice device, VkFormat colorFormat, VkFormat depthFormat,
                VkSampleCountFlagBits samples,
                VkDescriptorSetLayout sceneLayout,
                Assets::AssetManager& assets);
    void destroy() noexcept;

    void begin(VkCommandBuffer commandBuffer, VkFramebuffer framebuffer,
               VkExtent2D extent, VkDescriptorSet sceneDescriptorSet,
               VkBuffer vertexBuffer, VkBuffer instanceBuffer,
               VkBuffer indexBuffer) const;

    static void draw(VkCommandBuffer commandBuffer,
                     const Culling::IndexedIndirectDrawCount& indirectDraw);

    void drawOutline(VkCommandBuffer commandBuffer,
                     const Culling::IndexedIndirectDrawCount& indirectDraw) const;

    static void end(VkCommandBuffer commandBuffer);

    [[nodiscard]] VkRenderPass renderPass() const noexcept {
        return pipeline_.renderPass();
    }

private:
    GraphicsPipeline pipeline_;
    GraphicsPipeline outlinePipeline_;
};

} // namespace Engine
