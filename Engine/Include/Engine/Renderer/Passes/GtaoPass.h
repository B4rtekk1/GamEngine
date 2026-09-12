#pragma once

#include "Engine/Math/Mat4.h"
#include "Engine/Renderer/Vulkan/graphics_pipeline.h"
#include "Engine/Renderer/Vulkan/hdr_buffer.h"

#include <array>
#include <vulkan/vulkan.h>

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
    void draw(VkCommandBuffer commandBuffer, GraphicsPipeline& pipeline, VkFramebuffer framebuffer,
              VkDescriptorSet set, VkExtent2D extent, const void* constants, std::uint32_t constantSize);
    VkDevice device_ = VK_NULL_HANDLE;
    VkExtent2D fullExtent_{};
    VkExtent2D halfExtent_{};
    HdrBuffer raw_;
    HdrBuffer filtered_;
    std::array<HdrBuffer, 2> history_;
    HdrBuffer full_;
    std::array<GraphicsPipeline, 4> pipelines_;
    std::array<VkDescriptorSetLayout, 4> layouts_{};
    std::array<VkDescriptorPool, 4> pools_{};
    // Descriptor writes must not race command buffers submitted for earlier
    // frames.  This renderer has three frame slots.
    static constexpr std::uint32_t FramesInFlight = 3;
    std::array<std::array<VkDescriptorSet, FramesInFlight>, 4> sets_{};
    std::array<VkFramebuffer, 5> framebuffers_{};
    std::uint32_t historyIndex_ = 0;
    bool initialized_ = false;
};
} // namespace Engine
