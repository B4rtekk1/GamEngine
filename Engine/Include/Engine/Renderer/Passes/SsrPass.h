#pragma once

#include "Engine/Renderer/Culling/HiZBuffer.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace Engine {
namespace Assets {
class AssetManager;
}

/**
 * Inputs of the screen-space reflections Forward PBR traces per pixel.
 *
 * Forward walks each reflection ray through a closest-surface (MIN depth)
 * pyramid of this frame's opaque prepass with the Hi-Z traversal GlassPass
 * uses (Common/HiZTrace.slang), then reprojects the hit into the previous
 * frame and reads its lit colour from a mip-mapped snapshot, blurred by the
 * roughness cone. Both images are built with GlassPass's kernels: the
 * pyramid by hiz_initialize + glass_depth_reduce right after the prepass,
 * the snapshot by glass_snapshot_downsample once the frame is lit.
 */
class SsrPass final {
  public:
    struct CreateInfo {
        VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
        VkDevice device{VK_NULL_HANDLE};
        VmaAllocator allocator{VK_NULL_HANDLE};
        Assets::AssetManager* assets{nullptr};
        VkExtent2D extent{};
        /** Opaque prepass depth, sampled in DEPTH_STENCIL_READ_ONLY. */
        VkImageView depthView{VK_NULL_HANDLE};
        VkSampler depthSampler{VK_NULL_HANDLE};
        /** Lit HDR colour, sampled in SHADER_READ_ONLY. */
        VkImageView colorView{VK_NULL_HANDLE};
        VkSampler colorSampler{VK_NULL_HANDLE};
        /** Queue families that use the pyramid concurrently (async compute). */
        std::span<const std::uint32_t> sharingFamilies{};
    };

    /** Deepest pyramid level the trace visits (GLASS_HIZ_MAX_MIP / SSR_HIZ_MAX_MIP). */
    static constexpr std::uint32_t MaxPyramidLevel = 7;

    SsrPass() = default;
    ~SsrPass();
    SsrPass(const SsrPass&) = delete;
    SsrPass& operator=(const SsrPass&) = delete;

    void create(const CreateInfo& info);
    void destroy() noexcept;
    [[nodiscard]] bool created() const noexcept {
        return device_ != VK_NULL_HANDLE;
    }

    /**
     * Clears the snapshot to black and leaves both images in
     * SHADER_READ_ONLY, so Forward may bind them before the first trace.
     */
    void initialize(VkCommandBuffer commandBuffer);
    [[nodiscard]] bool initialized() const noexcept {
        return initialized_;
    }
    /**
     * Builds the pyramid from the prepass depth. The render graph imports
     * the pyramid and owns its transitions: it enters in GENERAL for storage
     * writes and leaves for the graph's final (sampled) state.
     */
    void recordPyramid(VkCommandBuffer commandBuffer);
    /** Stores the lit HDR colour (SHADER_READ_ONLY) as the next frame's reflected radiance. */
    void captureRadiance(VkCommandBuffer commandBuffer);
    /** Drops the radiance history, e.g. after a camera cut. */
    void reset() noexcept {
        radianceValid_ = false;
    }
    [[nodiscard]] bool radianceValid() const noexcept {
        return radianceValid_;
    }

    [[nodiscard]] const Culling::HiZBuffer& pyramid() const noexcept {
        return pyramid_;
    }
    [[nodiscard]] VkImageView radianceView() const noexcept {
        return snapshotView_;
    }
    [[nodiscard]] VkSampler radianceSampler() const noexcept {
        return snapshotSampler_;
    }
    [[nodiscard]] std::uint32_t radianceMipCount() const noexcept {
        return snapshotMips_;
    }

  private:
    void createImages();
    void createPipelines();
    void createDescriptors();

    CreateInfo info_{};
    std::vector<std::uint32_t> sharingFamilies_;
    VkDevice device_{VK_NULL_HANDLE};
    VkExtent2D extent_{};

    Culling::HiZBuffer pyramid_;
    std::uint32_t pyramidLastLevel_{0};

    VkImage snapshot_{VK_NULL_HANDLE};
    VmaAllocation snapshotMemory_{VK_NULL_HANDLE};
    VkImageView snapshotView_{VK_NULL_HANDLE};
    VkSampler snapshotSampler_{VK_NULL_HANDLE};
    std::uint32_t snapshotMips_{0};
    std::array<VkImageView, 8> snapshotMipViews_{};

    VkDescriptorSetLayout reduceSetLayout_{VK_NULL_HANDLE};
    VkPipelineLayout reducePipelineLayout_{VK_NULL_HANDLE};
    VkPipeline initializePipeline_{VK_NULL_HANDLE};
    VkPipeline reducePipeline_{VK_NULL_HANDLE};
    // Level 0 copies the depth (hiz_initialize); level i reduces level i - 1.
    std::vector<VkDescriptorSet> levelSets_;

    VkDescriptorSetLayout snapshotSetLayout_{VK_NULL_HANDLE};
    VkPipelineLayout snapshotPipelineLayout_{VK_NULL_HANDLE};
    VkPipeline snapshotPipeline_{VK_NULL_HANDLE};
    VkDescriptorSet snapshotSet_{VK_NULL_HANDLE};

    VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
    bool initialized_{false};
    bool radianceValid_{false};
};
} // namespace Engine
