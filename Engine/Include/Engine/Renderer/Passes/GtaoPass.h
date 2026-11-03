#pragma once

#include "Engine/Math/Mat4.h"
#include "Engine/Renderer/RenderConfig.h"
#include "Engine/Renderer/Textures/Texture2D.h"
#include "Engine/Renderer/Vulkan/hdr_buffer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace Engine {
namespace Assets {
class AssetManager;
}

struct GtaoExternalState {
    bool graphOwnsResultState = false;
};

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
                VkExtent2D fullExtent, VmaAllocator allocator, Assets::AssetManager& assets,
                GtaoQualitySettings quality = gtaoQualitySettings(GtaoQuality::High),
                std::span<const std::uint32_t> sharingFamilies = {});
    void destroy() noexcept;
    void reset() noexcept;
    /** Makes the white (unoccluded) bootstrap visibility sampleable before Forward reads it. */
    void initialize(VkCommandBuffer commandBuffer);
    [[nodiscard]] bool resultInitialized() const noexcept {
        return initialized_;
    }
    // `frameSlot` selects per-frame descriptors. `sampleIndex` drives the
    // stochastic sequence when a temporal accumulator is active; callers use
    // zero for a stable, non-temporal GTAO pattern.
    // GTAO deliberately has no history here: TAA is its temporal accumulator.
    void record(VkCommandBuffer commandBuffer, std::uint32_t frameSlot, std::uint32_t sampleIndex,
                VkImageView depthView, VkSampler depthSampler, VkImageView viewNormal, VkSampler viewNormalSampler,
                bool useExternalNormals, const Mat4& inverseProjection, GtaoExternalState externalState = {});
    [[nodiscard]] VkImage resultImage() const noexcept {
        return nativeResolution_ ? (quality_.denoisePassCount == 0 ? raw_.image() : filtered_.image()) : full_.image();
    }
    [[nodiscard]] VkFormat resultFormat() const noexcept {
        return resultFormat_;
    }
    [[nodiscard]] VkExtent2D resultExtent() const noexcept {
        return nativeResolution_ ? halfExtent_ : fullExtent_;
    }
    [[nodiscard]] VkImageView resultView() const noexcept {
        return nativeResolution_ ? (quality_.denoisePassCount == 0 ? raw_.imageView() : filtered_.imageView())
                                 : full_.imageView();
    }
    [[nodiscard]] VkSampler resultSampler() const noexcept {
        return nativeResolution_ ? (quality_.denoisePassCount == 0 ? raw_.sampler() : filtered_.sampler())
                                 : full_.sampler();
    }
    [[nodiscard]] VkImageView debugView(GtaoDebugView view) const noexcept {
        switch (view) {
        case GtaoDebugView::Raw:
            return raw_.imageView();
        case GtaoDebugView::Filtered:
            return quality_.denoisePassCount == 0 ? raw_.imageView() : filtered_.imageView();
        default:
            return resultView();
        }
    }
    [[nodiscard]] VkSampler debugSampler(GtaoDebugView view) const noexcept {
        switch (view) {
        case GtaoDebugView::Raw:
            return raw_.sampler();
        case GtaoDebugView::Filtered:
            return quality_.denoisePassCount == 0 ? raw_.sampler() : filtered_.sampler();
        default:
            return resultSampler();
        }
    }
    [[nodiscard]] VkImageLayout debugLayout(GtaoDebugView view) const noexcept {
        if (view == GtaoDebugView::Raw || (view == GtaoDebugView::Filtered && quality_.denoisePassCount == 0))
            return nativeResolution_ && quality_.denoisePassCount == 0 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                                       : VK_IMAGE_LAYOUT_GENERAL;
        if (view == GtaoDebugView::Filtered && !nativeResolution_)
            return VK_IMAGE_LAYOUT_GENERAL;
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

  private:
    // Descriptor writes must not race command buffers submitted for earlier
    // frames.  This renderer has three frame slots.
    static constexpr std::uint32_t FramesInFlight = 2;
    static constexpr std::uint32_t MaxBindings = 7;
    static constexpr std::uint32_t MaxDenoisePasses = 3;
    enum class Kernel : std::uint8_t { DepthPrefilter, Main, Denoise, Upsample, DenoiseUpsample };
    static constexpr std::size_t KernelCount = 5;
    struct ComputeKernel final {
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        std::array<VkDescriptorType, MaxBindings> bindingTypes{};
        std::uint32_t bindingCount = 0;
    };
    // Inputs only change when the pass is recreated (resize, quality change)
    // or the caller swaps its depth target, so each set remembers what it
    // holds and is rewritten only when that differs.
    struct DescriptorSlot final {
        VkDescriptorSet set = VK_NULL_HANDLE;
        std::array<VkDescriptorImageInfo, MaxBindings> written{};
        bool valid = false;
    };

    void clearImages(VkCommandBuffer commandBuffer, bool graphOwnsResultState = false);
    void createKernel(Kernel kernel, std::span<const VkDescriptorType> bindings, std::uint32_t pushConstantSize,
                      Assets::AssetManager& assets, const char* shader);
    void bindKernel(VkCommandBuffer commandBuffer, Kernel kernel, DescriptorSlot& slot,
                    std::span<const VkDescriptorImageInfo> images, const void* pushConstants,
                    std::uint32_t pushConstantSize);
    [[nodiscard]] ComputeKernel& kernel(Kernel kernel) noexcept {
        return kernels_[static_cast<std::size_t>(kernel)];
    }
    // Denoise owns one set per pass; every other kernel uses pass zero.
    [[nodiscard]] DescriptorSlot& slot(Kernel kernel, std::uint32_t frame, std::uint32_t pass = 0) noexcept {
        return slots_[static_cast<std::size_t>(kernel)][frame][pass];
    }

    VkDevice device_ = VK_NULL_HANDLE;
    PFN_vkCmdBeginDebugUtilsLabelEXT beginLabel_ = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT endLabel_ = nullptr;
    VkExtent2D fullExtent_{};
    VkExtent2D halfExtent_{};
    bool nativeResolution_ = false;
    VkFormat resultFormat_ = VK_FORMAT_UNDEFINED;
    GtaoQualitySettings quality_ = gtaoQualitySettings(GtaoQuality::High);
    HdrBuffer raw_;
    // Nearest-surface depth at the AO resolution, written by the depth
    // prefilter.  It is distinct from the far-biased hierarchy, which is used
    // only by horizon sampling.
    HdrBuffer baseDepth_;
    // The discontinuity mask guides the denoiser.
    HdrBuffer auxiliary_;
    HdrBuffer filtered_;
    // Intermediate AO for the second of up to three denoise passes.
    HdrBuffer scratch_;
    HdrBuffer full_;
    Texture2D hilbertLut_;
    // A dedicated view-depth hierarchy.  It intentionally is not Hi-Z:
    // GTAO consumes filtered view depths, while culling consumes extrema.
    VkImage linearDepthImage_ = VK_NULL_HANDLE;
    VkFormat linearDepthFormat_ = VK_FORMAT_UNDEFINED;
    VmaAllocation linearDepthAllocation_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkImageView linearDepthView_ = VK_NULL_HANDLE;
    std::vector<VkImageView> linearDepthMipViews_;
    VkSampler linearDepthSampler_ = VK_NULL_HANDLE;
    std::uint32_t linearDepthMipCount_ = 0;
    // Prefilter, main AO, 3x3 denoise, bilateral upsample and the fused
    // denoise/upsample are all compute; only the kernels the quality needs exist.
    std::array<ComputeKernel, KernelCount> kernels_{};
    std::array<std::array<std::array<DescriptorSlot, MaxDenoisePasses>, FramesInFlight>, KernelCount> slots_{};
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    bool initialized_ = false;
};
} // namespace Engine
