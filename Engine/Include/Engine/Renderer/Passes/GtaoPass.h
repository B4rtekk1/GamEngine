#pragma once

#include "Engine/Math/Mat4.h"
#include "Engine/Renderer/Vulkan/graphics_pipeline.h"
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
    void draw(VkCommandBuffer commandBuffer, GraphicsPipeline& pipeline, VkFramebuffer framebuffer,
              VkDescriptorSet set, VkExtent2D extent, const void* constants, std::uint32_t constantSize);
    VkDevice device_ = VK_NULL_HANDLE;
    VkExtent2D fullExtent_{};
    VkExtent2D halfExtent_{};
    // Descriptor writes must not race command buffers submitted for earlier
    // frames.  This renderer has three frame slots.
    static constexpr std::uint32_t FramesInFlight = 3;
    HdrBuffer raw_;
    // Linear depth and oct-encoded normal are shared, read-only guidance for
    // the spatial and upsample passes.  AO itself stays single-channel.
    HdrBuffer auxiliary_;
    HdrBuffer spatial_;
    HdrBuffer filtered_;
    std::array<HdrBuffer, 2> history_;
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
    std::array<GraphicsPipeline, 5> pipelines_;
    std::array<VkDescriptorSetLayout, 5> layouts_{};
    std::array<VkDescriptorPool, 5> pools_{};
    std::array<std::array<VkDescriptorSet, FramesInFlight>, 5> sets_{};
    std::array<VkFramebuffer, 6> framebuffers_{};
    std::uint32_t historyIndex_ = 0;
    bool initialized_ = false;
};
} // namespace Engine
