#pragma once

#include "Engine/Renderer/RenderConfig.h"
#include "Engine/Renderer/Vulkan/graphics_pipeline.h"
#include "Engine/Renderer/Vulkan/hdr_buffer.h"

#include <array>
#include <cstdint>
#include <vector>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace Engine {
namespace Assets {
class AssetManager;
}

/**
 * Turns the scene-referred HDR frame into the displayed image: metered auto
 * exposure, lens effects, colour grading and the display transform. It
 * draws into the swapchain (Player) or into an offscreen display image that
 * the editor shows as its Game View.
 */
class TonemapPass final {
  public:
    ~TonemapPass();

    TonemapPass() = default;

    TonemapPass(const TonemapPass&) = delete;

    TonemapPass& operator=(const TonemapPass&) = delete;

    /**
     * Source index 0 reads @p hdrView; 1 and 2 read the TAA history images and
     * the linear depth the resolve stored with each of them.
     */
    void create(VkPhysicalDevice physicalDevice, VkDevice device, VmaAllocator allocator, VkFormat swapchainFormat,
                VkExtent2D extent, const std::vector<VkImageView>& swapchainViews, VkImageView hdrView,
                VkSampler hdrSampler, VkImageView bloomView, Assets::AssetManager& assets,
                std::array<VkImageView, 2> temporalViews = {}, std::array<VkImageView, 2> temporalDepthViews = {},
                VkImageView velocityView = VK_NULL_HANDLE);

    void destroy() noexcept;

    /**
     * Meters the source image and advances the adapted luminance. Record it
     * before record()/recordDisplay() of the same frame, on the same queue.
     */
    void recordExposure(VkCommandBuffer commandBuffer, const PostProcessSettings& settings, float deltaTime,
                        std::uint32_t sourceIndex = 0);

    void record(VkCommandBuffer commandBuffer, std::uint32_t imageIndex, VkExtent2D extent,
                const PostProcessSettings& settings, std::uint32_t sourceIndex = 0);

    /** Draws into the offscreen display image and leaves it shader-readable. */
    void recordDisplay(VkCommandBuffer commandBuffer, VkExtent2D extent, const PostProcessSettings& settings,
                       std::uint32_t sourceIndex = 0);

    /** Offscreen display image in the swapchain format, for the editor's Game View. */
    [[nodiscard]] VkImageView displayView() const noexcept {
        return display_.imageView();
    }
    /** False until recordDisplay() has drawn (and transitioned) the image. */
    [[nodiscard]] bool displayReady() const noexcept {
        return displayInitialized_;
    }

  private:
    void draw(VkCommandBuffer commandBuffer, VkImageView target, VkExtent2D extent,
              const PostProcessSettings& settings, std::uint32_t sourceIndex);
    void initializeExposure(VkCommandBuffer commandBuffer);

    VkDevice device_ = VK_NULL_HANDLE;
    bool hasVelocity_ = false;
    bool hasTemporalDepth_ = false;
    GraphicsPipeline pipeline_;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 3> descriptorSets_{};
    std::vector<VkImageView> targetViews_;
    bool manualGamma_ = false;

    // Auto exposure: a 1x1 adapted luminance kept in GENERAL layout, written
    // by a single-workgroup compute kernel and read by the fragment shader.
    HdrBuffer exposure_;
    bool exposureInitialized_ = false;
    bool exposureMetered_ = false;
    VkDescriptorSetLayout exposureSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout exposurePipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline exposurePipeline_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 3> exposureSets_{};

    HdrBuffer display_;
    bool displayInitialized_ = false;
};
} // namespace Engine
