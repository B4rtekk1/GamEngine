#pragma once

#include "Engine/Renderer/Vulkan/graphics_pipeline.h"

#include <vulkan/vulkan.h>

#include <array>

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
                Assets::AssetManager& assets, bool velocity,
                const VkDescriptorImageInfo& opaqueColor, const VkDescriptorImageInfo& opaqueDepth);
    void destroy() noexcept;
    void begin(VkCommandBuffer commandBuffer, VkFramebuffer framebuffer, VkExtent2D extent,
               VkDescriptorSet sceneDescriptorSet, std::uint32_t frameIndex,
               VkBuffer vertexBuffer, VkBuffer indexBuffer) const;
    void draw(VkCommandBuffer commandBuffer, VkDescriptorSet descriptorSet,
              const Culling::IndexedIndirectDrawCount& indirectDraw,
              VkDeviceSize commandOffset, VkDeviceSize countOffset) const;
    static void end(VkCommandBuffer commandBuffer);
    [[nodiscard]] VkRenderPass renderPass() const noexcept { return pipeline_.renderPass(); }

private:
    static constexpr std::uint32_t FramesInFlight = 3;
    void createSceneDescriptors(const VkDescriptorImageInfo& opaqueColor,
                                const VkDescriptorImageInfo& opaqueDepth);
    GraphicsPipeline pipeline_;
    VkDevice device_{VK_NULL_HANDLE};
    VkDescriptorSetLayout sceneTextureLayout_{VK_NULL_HANDLE};
    VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, FramesInFlight> sceneTextureSets_{};
    bool hasVelocity_{};
};
} // namespace Engine
