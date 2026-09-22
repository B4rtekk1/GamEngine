#pragma once

#include "Engine/Math/Mat4.h"
#include "Engine/Renderer/Vulkan/hdr_buffer.h"
#include "Engine/Renderer/RenderConfig.h"
#include "Engine/Renderer/Textures/Texture2D.h"

#include <array>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <vector>

namespace Engine {
namespace Assets { class AssetManager; }

/**
 * Screen-space ground-truth ambient occlusion.  The pass deliberately writes
 * visibility, not shaded colour: Forward PBR consumes it only for IBL.
 */
class GtaoPass final {
public:
    ~GtaoPass();
    GtaoPass() = default;
    GtaoPass(const GtaoPass&) = delete;
    GtaoPass& operator=(const GtaoPass&) = delete;

    void create(VkPhysicalDevice physicalDevice, VkDevice device, VkCommandPool commandPool, VkQueue queue,
                VkExtent2D fullExtent,
                VmaAllocator allocator, Assets::AssetManager& assets,
                GtaoQualitySettings quality = gtaoQualitySettings(GtaoQuality::High));
    void destroy() noexcept;
    void reset() noexcept;
    /** Makes the white (unoccluded) bootstrap visibility sampleable before Forward reads it. */
    void initialize(VkCommandBuffer commandBuffer);
    // `frameSlot` selects per-frame descriptors. `sampleIndex` drives the
    // stochastic sequence when a temporal accumulator is active; callers use
    // zero for a stable, non-temporal GTAO pattern.
    // GTAO deliberately has no history here: TAA is its temporal accumulator.
    void record(VkCommandBuffer commandBuffer, std::uint32_t frameSlot,
                std::uint32_t sampleIndex, VkImageView depthView,
                VkSampler depthSampler, VkImageView viewNormal,
                VkSampler viewNormalSampler, bool useExternalNormals, const Mat4& inverseProjection);
    [[nodiscard]] VkImageView resultView() const noexcept {
        return nativeResolution_ ? filtered_.imageView() : full_.imageView();
    }
    [[nodiscard]] VkSampler resultSampler() const noexcept {
        return nativeResolution_ ? filtered_.sampler() : full_.sampler();
    }
    [[nodiscard]] VkImageView debugView(GtaoDebugView view) const noexcept {
        switch (view) {
        case GtaoDebugView::Raw: return raw_.imageView();
        case GtaoDebugView::Filtered: return filtered_.imageView();
        default: return resultView();
        }
    }
    [[nodiscard]] VkSampler debugSampler(GtaoDebugView view) const noexcept {
        switch (view) {
        case GtaoDebugView::Raw: return raw_.sampler();
        case GtaoDebugView::Filtered: return filtered_.sampler();
        default: return resultSampler();
        }
    }
    [[nodiscard]] VkImageLayout debugLayout(GtaoDebugView view) const noexcept {
        if (view == GtaoDebugView::Raw) return VK_IMAGE_LAYOUT_GENERAL;
        if (view == GtaoDebugView::Filtered && !nativeResolution_) return VK_IMAGE_LAYOUT_GENERAL;
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

private:
    void clearImages(VkCommandBuffer commandBuffer);
    void buildLinearDepth(VkCommandBuffer commandBuffer, std::uint32_t frameIndex, VkImageView depthView, VkSampler depthSampler,
                          const Mat4& inverseProjection);
    VkDevice device_ = VK_NULL_HANDLE;
    VkExtent2D fullExtent_{};
    VkExtent2D halfExtent_{};
    bool nativeResolution_ = false;
    GtaoQualitySettings quality_ = gtaoQualitySettings(GtaoQuality::High);
    // Descriptor writes must not race command buffers submitted for earlier
    // frames.  This renderer has three frame slots.
    static constexpr std::uint32_t FramesInFlight = 2;
    HdrBuffer raw_;
    // Nearest-surface depth at the AO resolution.  It is distinct from the
    // far-biased hierarchy, which is used only by horizon sampling.
    HdrBuffer baseDepth_;
    // The discontinuity mask guides the denoiser; its depth is the
    // representative half-resolution baseDepth_ above.
    HdrBuffer auxiliary_;
    HdrBuffer filtered_;
    HdrBuffer full_;
    Texture2D hilbertLut_;
    // A dedicated R32F view-depth hierarchy.  It intentionally is not Hi-Z:
    // GTAO consumes filtered view depths, while culling consumes extrema.
    VkImage linearDepthImage_ = VK_NULL_HANDLE;
    VkFormat linearDepthFormat_ = VK_FORMAT_UNDEFINED;
    VmaAllocation linearDepthAllocation_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkImageView linearDepthView_ = VK_NULL_HANDLE;
    std::vector<VkImageView> linearDepthMipViews_;
    VkSampler linearDepthSampler_ = VK_NULL_HANDLE;
    std::uint32_t linearDepthMipCount_ = 0;
    bool linearDepthInitialized_ = false;
    VkDescriptorSetLayout depthLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout depthPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline depthPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool depthDescriptorPool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, FramesInFlight> depthSets_{};
    // Main AO, 5x5 denoise and bilateral upsample are all compute pipelines.
    std::array<VkDescriptorSetLayout, 3> computeLayouts_{};
    std::array<VkPipelineLayout, 3> computePipelineLayouts_{};
    std::array<VkPipeline, 3> computePipelines_{};
    std::array<VkPipeline, 4> mainQualityPipelines_{};
    VkDescriptorPool computeDescriptorPool_ = VK_NULL_HANDLE;
    std::array<std::array<VkDescriptorSet, FramesInFlight>, 3> computeSets_{};
    std::array<std::array<std::vector<VkDescriptorImageInfo>, FramesInFlight>, 3> computeDescriptorCache_{};
    bool initialized_ = false;
};
} // namespace Engine
