#pragma once

#include "Engine/Renderer/Vulkan/buffer.h"

#include <array>
#include <span>
#include <vulkan/vulkan.h>

namespace Engine {
namespace Assets {
class AssetManager;
}

/**
 * Depth-bounded Forward+ light lists. After the depth prepass, every 16x16
 * pixel tile keeps the local lights whose range reaches the view-space box
 * between its nearest and farthest depth; Game View forward shading walks
 * those instead of its far looser log-depth cluster list.
 */
class TileLightCullingPass final {
  public:
    ~TileLightCullingPass();
    TileLightCullingPass() = default;
    TileLightCullingPass(const TileLightCullingPass&) = delete;
    TileLightCullingPass& operator=(const TileLightCullingPass&) = delete;

    /** Without `culling` only the list buffers exist (bound, never read). */
    void create(VkDevice device, VkExtent2D extent, VmaAllocator allocator, Assets::AssetManager& assets,
                std::span<const VkBuffer> frameUniformBuffers, bool culling);
    void destroy() noexcept;
    [[nodiscard]] bool culling() const noexcept {
        return pipeline_ != VK_NULL_HANDLE;
    }
    /** Leaves the frame slot's lists readable by the fragment stage. */
    void record(VkCommandBuffer commandBuffer, std::uint32_t frameSlot, VkImageView depth, VkSampler depthSampler,
                VkBuffer localLights);
    [[nodiscard]] VkBuffer lists(std::uint32_t frameSlot) const noexcept {
        return lists_[frameSlot % lists_.size()].handle();
    }
    [[nodiscard]] VkExtent2D extent() const noexcept {
        return extent_;
    }

  private:
    VkDevice device_{VK_NULL_HANDLE};
    VkExtent2D extent_{};
    std::array<Buffer, 2> lists_{};
    VkDescriptorSetLayout layout_{VK_NULL_HANDLE};
    VkDescriptorPool pool_{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, 2> sets_{};
    VkPipelineLayout pipelineLayout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
};
} // namespace Engine
