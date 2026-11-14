#pragma once

#include "Engine/Renderer/Vulkan/hdr_buffer.h"

#include <array>
#include <span>
#include <vulkan/vulkan.h>

namespace Engine {
namespace Assets {
class AssetManager;
}

/**
 * Deferred local lighting for Game View. The lighting pass's PBR programs
 * write their shading inputs to surfaceData() instead of walking the local
 * lights per fragment; record() then evaluates the lights once per visible
 * pixel from the depth-bounded tile lists and adds them to the HDR target.
 */
class DeferredLocalLightingPass final {
  public:
    static constexpr VkFormat SurfaceFormat = VK_FORMAT_R32G32B32A32_UINT;

    struct Inputs final {
        VkImageView depth{VK_NULL_HANDLE};
        VkSampler depthSampler{VK_NULL_HANDLE};
        VkImage hdr{VK_NULL_HANDLE};
        VkImageView hdrView{VK_NULL_HANDLE};
        VkBuffer localLights{VK_NULL_HANDLE};
        VkBuffer tileLights{VK_NULL_HANDLE};
        VkImageView localShadows{VK_NULL_HANDLE};
        VkSampler localShadowSampler{VK_NULL_HANDLE};
        /**
         * Stochastic (MegaLights-style) mode: the tracer's four light samples
         * per quad and their RIS weights (LocalLightShadowPass, mode 1), in
         * GENERAL layout. Without them every tile light is evaluated.
         */
        VkImageView lightSamples{VK_NULL_HANDLE};
        VkImageView lightSampleWeights{VK_NULL_HANDLE};
    };

    ~DeferredLocalLightingPass();
    DeferredLocalLightingPass() = default;
    DeferredLocalLightingPass(const DeferredLocalLightingPass&) = delete;
    DeferredLocalLightingPass& operator=(const DeferredLocalLightingPass&) = delete;

    void create(VkPhysicalDevice physicalDevice, VkDevice device, VkExtent2D extent, VmaAllocator allocator,
                Assets::AssetManager& assets, std::span<const VkBuffer> frameUniformBuffers,
                bool stochastic = true);
    void destroy() noexcept;
    [[nodiscard]] bool ready() const noexcept {
        return pipeline_ != VK_NULL_HANDLE;
    }
    [[nodiscard]] bool stochasticReady() const noexcept {
        return shadePipeline_ != VK_NULL_HANDLE && temporalPipeline_ != VK_NULL_HANDLE;
    }
    [[nodiscard]] VkImageView surfaceData() const noexcept {
        return surface_.imageView();
    }
    [[nodiscard]] VkImage surfaceImage() const noexcept {
        return surface_.image();
    }
    /** Before the lighting pass: the surface target becomes a color attachment. */
    void prepare(VkCommandBuffer commandBuffer) const;
    /**
     * After the lighting pass (HDR and surface target in
     * COLOR_ATTACHMENT_OPTIMAL): adds the local lights; leaves both images
     * in COLOR_ATTACHMENT_OPTIMAL again for what follows.
     */
    void record(VkCommandBuffer commandBuffer, std::uint32_t frameSlot, const Inputs& inputs);
    [[nodiscard]] VkExtent2D extent() const noexcept {
        return extent_;
    }

  private:
    VkDevice device_{VK_NULL_HANDLE};
    VkExtent2D extent_{};
    HdrBuffer surface_;
    VkDescriptorSetLayout layout_{VK_NULL_HANDLE};
    VkDescriptorPool pool_{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, 2> sets_{};
    VkPipelineLayout pipelineLayout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};

    // Stochastic mode: shading and temporal accumulation.
    void createStochastic(VkPhysicalDevice physicalDevice, VmaAllocator allocator, Assets::AssetManager& assets,
                          std::span<const VkBuffer> frameUniformBuffers);
    void recordStochastic(VkCommandBuffer commandBuffer, std::uint32_t frameSlot, const Inputs& inputs);
    HdrBuffer noisyDiffuse_;
    HdrBuffer noisySpecular_;
    std::array<HdrBuffer, 2> historyDiffuse_{};
    std::array<HdrBuffer, 2> historySpecular_{};
    std::array<bool, 2> historyDefined_{};
    VkDescriptorSetLayout shadeLayout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout temporalLayout_{VK_NULL_HANDLE};
    VkDescriptorPool stochasticPool_{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, 2> shadeSets_{};
    std::array<VkDescriptorSet, 2> temporalSets_{};
    VkPipelineLayout shadePipelineLayout_{VK_NULL_HANDLE};
    VkPipelineLayout temporalPipelineLayout_{VK_NULL_HANDLE};
    VkPipeline shadePipeline_{VK_NULL_HANDLE};
    VkPipeline temporalPipeline_{VK_NULL_HANDLE};
};
} // namespace Engine
