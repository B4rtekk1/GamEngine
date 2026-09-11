#pragma once

#include "Engine/Renderer/RenderGraph/RenderGraph.h"

#include <vector>

namespace Engine::RenderGraph {

    /**
     * Vulkan submission backend for a compiled RenderGraph. It owns a timeline
     * semaphore and records one primary command buffer per graph submission.
     * Call beginFrame() only after the fence supplied to submit() has signaled.
     */
    class RenderGraphExecutor final {
    public:
        RenderGraphExecutor() = default;
        ~RenderGraphExecutor() { destroy(); }
        RenderGraphExecutor(const RenderGraphExecutor&) = delete;
        RenderGraphExecutor& operator=(const RenderGraphExecutor&) = delete;

        void initialize(VkDevice device, VkQueue graphicsQueue, std::uint32_t graphicsFamily,
                        VkQueue computeQueue, std::uint32_t computeFamily);
        void beginFrame();
        /** Compiles the graph and records releases from the legacy graphics prelude. */
        void recordPreludeReleases(RenderGraph& graph, VkCommandBuffer commandBuffer);
        /** Submit legacy graphics work that must complete before graph roots. */
        [[nodiscard]] std::uint64_t submitPrelude(VkCommandBuffer commandBuffer);
        void submit(RenderGraph& graph, VkSemaphore imageAvailable = VK_NULL_HANDLE,
                    VkPipelineStageFlags2 imageAvailableStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VkSemaphore renderFinished = VK_NULL_HANDLE, VkFence completionFence = VK_NULL_HANDLE,
                    std::uint64_t preludeValue = 0);
        void destroy() noexcept;

        [[nodiscard]] VkSemaphore timeline() const noexcept { return timeline_; }
        [[nodiscard]] std::uint64_t lastSubmittedValue() const noexcept { return nextTimelineValue_ - 1; }

    private:
        [[nodiscard]] VkCommandPool poolFor(QueueClass queue) const noexcept;
        [[nodiscard]] VkQueue queueFor(QueueClass queue) const noexcept;

        VkDevice device_{VK_NULL_HANDLE};
        VkQueue graphicsQueue_{VK_NULL_HANDLE};
        VkQueue computeQueue_{VK_NULL_HANDLE};
        VkCommandPool graphicsPool_{VK_NULL_HANDLE};
        VkCommandPool computePool_{VK_NULL_HANDLE};
        VkSemaphore timeline_{VK_NULL_HANDLE};
        std::uint32_t graphicsFamily_{VK_QUEUE_FAMILY_IGNORED};
        std::uint32_t computeFamily_{VK_QUEUE_FAMILY_IGNORED};
        std::uint64_t nextTimelineValue_{1};
        bool hasPendingWork_{};
        std::vector<VkCommandBuffer> commandBuffers_;
    };
} // namespace Engine::RenderGraph
