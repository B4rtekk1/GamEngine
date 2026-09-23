#pragma once

#include "Engine/Renderer/Vulkan/hdr_buffer.h"

#include <array>
#include <vulkan/vulkan.h>

namespace Engine {
    namespace Assets { class AssetManager; }

    /** Resolves a jittered HDR frame against a ping-pong temporal history. */
    class TemporalAaPass final {
    public:
        ~TemporalAaPass();
        TemporalAaPass() = default;
        TemporalAaPass(const TemporalAaPass&) = delete;
        TemporalAaPass& operator=(const TemporalAaPass&) = delete;

        void create(VkPhysicalDevice physicalDevice, VkDevice device, VkExtent2D extent,
                    VmaAllocator allocator, VkImageView currentView, VkSampler sampler,
                    VkImageView velocityView, VkSampler velocitySampler,
                    VkImageView currentDepthView, VkSampler currentDepthSampler,
                    VkImageView waterVelocityView, VkSampler waterVelocitySampler,
                    VkImageView waterMetaView, VkSampler waterMetaSampler,
                    VkImageView waterSurfaceView, VkSampler waterSurfaceSampler,
                    Assets::AssetManager& assets);
        void destroy() noexcept;
        void reset() noexcept;
        /** Clears newly-created histories before the render graph takes ownership. */
        void prepareHistory(VkCommandBuffer commandBuffer);
        void setVirtualWaterEnabled(bool enabled) noexcept { virtualWaterEnabled_ = enabled; }
        void record(VkCommandBuffer commandBuffer, VkExtent2D extent,
                    float currentJitterX, float currentJitterY,
                    float projectionA, float projectionB);
        [[nodiscard]] VkImageView resolvedView() const noexcept { return history_[historyIndex_].imageView(); }
        /** Image backing resolvedView(); exposed for render-graph declarations. */
        [[nodiscard]] VkImage resolvedImage() const noexcept { return history_[historyIndex_].image(); }
        /** Image selected by a temporal-history descriptor index. */
        [[nodiscard]] VkImage historyImage(std::uint32_t index) const noexcept {
            return history_[index % history_.size()].image();
        }
        [[nodiscard]] std::uint32_t resolvedIndex() const noexcept { return historyIndex_; }
        // The editor builds its ImGui draw data before the command buffer is
        // recorded. This is the history image that record() will write later
        // in that command buffer, before ImGui samples it.
        [[nodiscard]] std::uint32_t nextResolvedIndex() const noexcept { return 1U - historyIndex_; }
        [[nodiscard]] std::array<VkImageView, 2> historyViews() const noexcept {
            return {history_[0].imageView(), history_[1].imageView()};
        }

    private:
        void initializeHistory(VkCommandBuffer commandBuffer);
        VkDevice device_ = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
        VkPipeline pipeline_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
        VkDescriptorPool pool_ = VK_NULL_HANDLE;
        // Two history inputs times two source variants: ordinary scene data
        // uses current HDR as a valid fallback for the water-only bindings.
        std::array<VkDescriptorSet, 4> sets_{};
        std::array<HdrBuffer, 2> history_;
        // Bounded-color feedback is separate from the HDR image consumed by bloom and presentation.
        std::array<HdrBuffer, 2> historyColor_;
        // A sampled copy of the depth that produced each HDR history image.
        // It is stored by the resolve itself, so the two ping-pong histories
        // always refer to exactly the same frame.
        std::array<HdrBuffer, 2> historyDepth_;
        std::uint32_t historyIndex_ = 0;
        bool initialized_ = false;
        bool historyValid_ = false;
        float previousJitterX_ = 0.0F;
        float previousJitterY_ = 0.0F;
        bool virtualWaterEnabled_ = false;
    };
} // namespace Engine
