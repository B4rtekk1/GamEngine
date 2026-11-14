#pragma once

#include "Engine/Renderer/Vulkan/graphics_pipeline.h"
#include "Engine/Renderer/Vulkan/hdr_buffer.h"

#include <array>
#include <cstdint>
#include <vulkan/vulkan.h>

namespace Engine {
namespace Assets {
class AssetManager;
}

/**
 * Glow of bright linear-HDR pixels: thresholds the frame at half resolution,
 * filters it down a chain of LevelCount mips and sums the chain back up, so
 * the result holds every radius from a tight halo to a wide soft glow.
 */
class BloomPass final {
  public:
    ~BloomPass();
    BloomPass() = default;
    BloomPass(const BloomPass&) = delete;
    BloomPass& operator=(const BloomPass&) = delete;
    void create(VkPhysicalDevice physicalDevice, VkDevice device, VkExtent2D sourceExtent, VmaAllocator allocator,
                Assets::AssetManager& assets);
    void destroy() noexcept;
    /** Clears a newly-created result into its sampled layout before the render graph takes ownership. */
    void prepare(VkCommandBuffer commandBuffer);
    void record(VkCommandBuffer commandBuffer, VkImageView source, VkSampler sourceSampler, std::uint32_t frameIndex);
    [[nodiscard]] VkImageView resultView() const noexcept {
        return result_.imageView();
    }
    /** Image backing resultView(); exposed for render-graph declarations. */
    [[nodiscard]] VkImage resultImage() const noexcept {
        return result_.image();
    }
    [[nodiscard]] VkSampler resultSampler() const noexcept {
        return result_.sampler();
    }

  private:
    static constexpr std::uint32_t LevelCount = 6;
    static constexpr std::uint32_t FramesInFlight = 2;
    void drawLevel(VkCommandBuffer commandBuffer, const GraphicsPipeline& pipeline, VkDescriptorSet set,
                   VkImageView target, VkExtent2D extent, const void* constants, std::uint32_t constantsSize) const;

    VkDevice device_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    // down_[0] is the thresholded half-resolution frame; each next level halves.
    std::array<HdrBuffer, LevelCount> down_{};
    std::array<VkExtent2D, LevelCount> levelExtents_{};
    // up_[0] is unused: result_ is the top of the upward chain.
    std::array<HdrBuffer, LevelCount - 1> up_{};
    HdrBuffer result_;
    GraphicsPipeline pipeline_;
    GraphicsPipeline upPipeline_;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    // Per frame slot: the first step reads the (alternating) TAA output.
    std::array<VkDescriptorSet, FramesInFlight> sets_{};
    std::array<VkDescriptorSet, LevelCount - 1> downSets_{};
    std::array<VkDescriptorSet, LevelCount - 1> upSets_{};
    // A frame slot is fence-safe when record() is called.  Keep its last
    // binding so steady-state post processing performs no descriptor write.
    std::array<VkDescriptorImageInfo, FramesInFlight> cachedSources_{};
    std::array<bool, FramesInFlight> sourceDescriptorsValid_{};
    bool initialized_ = false;
};
} // namespace Engine
