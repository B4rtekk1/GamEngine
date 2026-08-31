#pragma once

#include "Engine/Renderer/Vulkan/graphics_pipeline.h"
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
                    Assets::AssetManager& assets);
        void destroy() noexcept;
        void reset() noexcept;
        void record(VkCommandBuffer commandBuffer, VkExtent2D extent,
                    float currentJitterX, float currentJitterY);
        [[nodiscard]] VkImageView resolvedView() const noexcept { return history_[historyIndex_].imageView(); }
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
        GraphicsPipeline pipeline_;
        VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
        VkDescriptorPool pool_ = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, 2> sets_{};
        std::array<VkFramebuffer, 2> framebuffers_{};
        std::array<HdrBuffer, 2> history_;
        std::uint32_t historyIndex_ = 0;
        bool initialized_ = false;
        bool historyValid_ = false;
        float previousJitterX_ = 0.0F;
        float previousJitterY_ = 0.0F;
    };
} // namespace Engine
