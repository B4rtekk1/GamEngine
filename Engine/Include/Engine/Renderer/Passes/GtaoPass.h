#pragma once

#include "Engine/Math/Mat4.h"
#include "Engine/Renderer/Vulkan/hdr_buffer.h"

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

    void create(VkPhysicalDevice physicalDevice, VkDevice device, VkExtent2D fullExtent,
                VmaAllocator allocator, Assets::AssetManager& assets);
    void destroy() noexcept;
    void reset() noexcept;
    /** Makes the white (unoccluded) bootstrap visibility sampleable before Forward reads it. */
    void initialize(VkCommandBuffer commandBuffer);
    void record(VkCommandBuffer commandBuffer, std::uint32_t frameIndex,
                VkImageView depthView, VkSampler depthSampler,
                VkImageView velocityView, VkSampler velocitySampler,
                const Mat4& inverseProjection, bool useTemporalVelocity);
    [[nodiscard]] VkImageView resultView() const noexcept { return full_.imageView(); }
    [[nodiscard]] VkSampler resultSampler() const noexcept { return full_.sampler(); }

private:
    void clearImages(VkCommandBuffer commandBuffer);
    void buildLinearDepth(VkCommandBuffer commandBuffer, std::uint32_t frameIndex, VkImageView depthView, VkSampler depthSampler,
                          const Mat4& inverseProjection);
    VkDevice device_ = VK_NULL_HANDLE;
    VkExtent2D fullExtent_{};
    VkExtent2D halfExtent_{};
    // Descriptor writes must not race command buffers submitted for earlier
    // frames.  This renderer has three frame slots.
    static constexpr std::uint32_t FramesInFlight = 3;
    HdrBuffer raw_;
    // Linear depth, oct-normal and a discontinuity mask guide the 5x5
    // groupshared denoiser and the bilateral upsample.
    HdrBuffer auxiliary_;
    HdrBuffer filtered_;
    HdrBuffer full_;
    // A dedicated R16F view-depth hierarchy.  It intentionally is not Hi-Z:
    // GTAO consumes filtered view depths, while culling consumes extrema.
    VkImage linearDepthImage_ = VK_NULL_HANDLE;
    VmaAllocation linearDepthAllocation_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkImageView linearDepthView_ = VK_NULL_HANDLE;
    std::vector<VkImageView> linearDepthMipViews_;
    VkSampler linearDepthSampler_ = VK_NULL_HANDLE;
    std::uint32_t linearDepthMipCount_ = 0;
    bool linearDepthInitialized_ = false;
    std::array<VkDescriptorSetLayout, 2> depthLayouts_{};
    std::array<VkPipelineLayout, 2> depthPipelineLayouts_{};
    std::array<VkPipeline, 2> depthPipelines_{};
    VkDescriptorPool depthDescriptorPool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, FramesInFlight> linearizeSets_{};
    std::vector<VkDescriptorSet> depthReduceSets_;
    // Main AO, 5x5 denoise and bilateral upsample are all compute pipelines.
    std::array<VkDescriptorSetLayout, 3> computeLayouts_{};
    std::array<VkPipelineLayout, 3> computePipelineLayouts_{};
    std::array<VkPipeline, 3> computePipelines_{};
    VkDescriptorPool computeDescriptorPool_ = VK_NULL_HANDLE;
    std::array<std::array<VkDescriptorSet, FramesInFlight>, 3> computeSets_{};
    bool initialized_ = false;
};
} // namespace Engine
