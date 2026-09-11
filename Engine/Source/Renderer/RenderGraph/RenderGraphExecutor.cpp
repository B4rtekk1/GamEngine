#include "Engine/Renderer/RenderGraph/RenderGraphExecutor.h"

#include <algorithm>
#include <stdexcept>

namespace Engine::RenderGraph {
    void RenderGraphExecutor::initialize(const VkDevice device, const VkQueue graphicsQueue,
                                         const std::uint32_t graphicsFamily, const VkQueue computeQueue,
                                         const std::uint32_t computeFamily) {
        if (device == VK_NULL_HANDLE || graphicsQueue == VK_NULL_HANDLE || computeQueue == VK_NULL_HANDLE)
            throw std::invalid_argument("RenderGraphExecutor requires valid Vulkan queues");
        if (device_ != VK_NULL_HANDLE) throw std::logic_error("RenderGraphExecutor is already initialized");
        device_ = device; graphicsQueue_ = graphicsQueue; computeQueue_ = computeQueue;
        graphicsFamily_ = graphicsFamily; computeFamily_ = computeFamily;
        const auto createPool = [this](const std::uint32_t family, VkCommandPool& result) {
            const VkCommandPoolCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
            if (vkCreateCommandPool(device_, &info, nullptr, &result) != VK_SUCCESS)
                throw std::runtime_error("Could not create RenderGraph command pool");
        };
        try {
            createPool(graphicsFamily, graphicsPool_);
            if (computeFamily != graphicsFamily) createPool(computeFamily, computePool_);
            const VkSemaphoreTypeCreateInfo timelineType{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
            const VkSemaphoreCreateInfo semaphoreInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &timelineType};
            if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &timeline_) != VK_SUCCESS)
                throw std::runtime_error("Could not create RenderGraph timeline semaphore");
        } catch (...) { destroy(); throw; }
    }

    void RenderGraphExecutor::beginFrame() {
        if (device_ == VK_NULL_HANDLE) throw std::logic_error("RenderGraphExecutor is not initialized");
        // The caller's frame fence is the proof all recorded command buffers
        // completed. Resetting earlier would invalidate in-flight work.
        if (hasPendingWork_) {
            if (vkResetCommandPool(device_, graphicsPool_, 0) != VK_SUCCESS ||
                (computePool_ != VK_NULL_HANDLE && vkResetCommandPool(device_, computePool_, 0) != VK_SUCCESS))
                throw std::runtime_error("Could not reset RenderGraph command pool");
            commandBuffers_.clear();
            hasPendingWork_ = false;
        }
    }

    void RenderGraphExecutor::recordPreludeReleases(RenderGraph& graph, const VkCommandBuffer commandBuffer) {
        if (device_ == VK_NULL_HANDLE) throw std::logic_error("RenderGraphExecutor is not initialized");
        graph.setQueueFamilies(graphicsFamily_, computeFamily_);
        graph.compile();
        graph.recordExternalReleases(commandBuffer);
    }

    VkCommandPool RenderGraphExecutor::poolFor(const QueueClass queue) const noexcept {
        return queue == QueueClass::AsyncCompute && computePool_ != VK_NULL_HANDLE ? computePool_ : graphicsPool_;
    }
    VkQueue RenderGraphExecutor::queueFor(const QueueClass queue) const noexcept {
        return queue == QueueClass::AsyncCompute ? computeQueue_ : graphicsQueue_;
    }

    std::uint64_t RenderGraphExecutor::submitPrelude(const VkCommandBuffer commandBuffer) {
        if (device_ == VK_NULL_HANDLE) throw std::logic_error("RenderGraphExecutor is not initialized");
        if (hasPendingWork_) throw std::logic_error("Call beginFrame after the previous completion fence before submitPrelude");
        if (commandBuffer == VK_NULL_HANDLE) throw std::invalid_argument("RenderGraph prelude command buffer is null");
        const std::uint64_t value = nextTimelineValue_++;
        const VkCommandBufferSubmitInfo command{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                                 .commandBuffer = commandBuffer};
        const VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = timeline_, .value = value, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        const VkSubmitInfo2 submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .commandBufferInfoCount = 1, .pCommandBufferInfos = &command,
            .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &signal};
        if (vkQueueSubmit2(graphicsQueue_, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS)
            throw std::runtime_error("Could not submit RenderGraph prelude");
        return value;
    }

    void RenderGraphExecutor::submit(RenderGraph& graph, const VkSemaphore imageAvailable,
                                     const VkPipelineStageFlags2 imageAvailableStage,
                                     const VkSemaphore renderFinished, const VkFence completionFence,
                                     const std::uint64_t preludeValue) {
        if (device_ == VK_NULL_HANDLE) throw std::logic_error("RenderGraphExecutor is not initialized");
        if (hasPendingWork_) throw std::logic_error("Call beginFrame after the previous completion fence before submit");
        graph.setQueueFamilies(graphicsFamily_, computeFamily_);
        graph.compile();
        const auto& plan = graph.submissionPlan();
        if (plan.empty()) return;
        commandBuffers_.resize(plan.size());
        for (std::uint32_t batch = 0; batch < plan.size(); ++batch) {
            const VkCommandBufferAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = poolFor(plan[batch].queue), .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
            if (vkAllocateCommandBuffers(device_, &allocation, &commandBuffers_[batch]) != VK_SUCCESS)
                throw std::runtime_error("Could not allocate RenderGraph command buffer");
            const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            if (vkBeginCommandBuffer(commandBuffers_[batch], &begin) != VK_SUCCESS) throw std::runtime_error("Could not begin RenderGraph command buffer");
            graph.recordBatch(batch, commandBuffers_[batch]);
            if (vkEndCommandBuffer(commandBuffers_[batch]) != VK_SUCCESS) throw std::runtime_error("Could not end RenderGraph command buffer");
        }
        const auto firstGraphics = std::ranges::find_if(plan, [](const SubmissionBatch& batch) { return batch.queue == QueueClass::Graphics; });
        const auto lastGraphics = std::find_if(plan.rbegin(), plan.rend(), [](const SubmissionBatch& batch) { return batch.queue == QueueClass::Graphics; });
        for (std::uint32_t batch = 0; batch < plan.size(); ++batch) {
            std::vector<VkSemaphoreSubmitInfo> waits;
            // Every root can observe data recorded by the legacy prelude. This
            // keeps independent graphics and compute roots eligible to overlap
            // as soon as that prelude has completed.
            if (preludeValue != 0 && plan[batch].waits.empty())
                waits.push_back({.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = timeline_,
                    .value = preludeValue, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT});
            for (const SubmissionWait& wait : plan[batch].waits)
                waits.push_back({.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = timeline_,
                    .value = nextTimelineValue_ + wait.producerBatch, .stageMask = wait.stageMask});
            if (imageAvailable != VK_NULL_HANDLE && firstGraphics != plan.end() && batch == static_cast<std::uint32_t>(firstGraphics - plan.begin()))
                waits.push_back({.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = imageAvailable, .stageMask = imageAvailableStage});
            std::vector<VkSemaphoreSubmitInfo> signals{{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .semaphore = timeline_, .value = nextTimelineValue_ + batch, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
            if (renderFinished != VK_NULL_HANDLE && lastGraphics != plan.rend() && batch == static_cast<std::uint32_t>((plan.rend() - 1) - lastGraphics))
                signals.push_back({.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = renderFinished, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT});
            const VkCommandBufferSubmitInfo command{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = commandBuffers_[batch]};
            const VkSubmitInfo2 submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .waitSemaphoreInfoCount = static_cast<std::uint32_t>(waits.size()),
                .pWaitSemaphoreInfos = waits.data(), .commandBufferInfoCount = 1, .pCommandBufferInfos = &command,
                .signalSemaphoreInfoCount = static_cast<std::uint32_t>(signals.size()), .pSignalSemaphoreInfos = signals.data()};
            const VkFence fence = batch + 1 == plan.size() ? completionFence : VK_NULL_HANDLE;
            if (vkQueueSubmit2(queueFor(plan[batch].queue), 1, &submit, fence) != VK_SUCCESS)
                throw std::runtime_error("Could not submit RenderGraph batch");
        }
        nextTimelineValue_ += plan.size();
        hasPendingWork_ = true;
    }

    void RenderGraphExecutor::destroy() noexcept {
        if (device_ == VK_NULL_HANDLE) return;
        if (timeline_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, timeline_, nullptr);
        if (computePool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, computePool_, nullptr);
        if (graphicsPool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, graphicsPool_, nullptr);
        device_ = VK_NULL_HANDLE; graphicsQueue_ = VK_NULL_HANDLE; computeQueue_ = VK_NULL_HANDLE;
        graphicsFamily_ = VK_QUEUE_FAMILY_IGNORED; computeFamily_ = VK_QUEUE_FAMILY_IGNORED;
        graphicsPool_ = VK_NULL_HANDLE; computePool_ = VK_NULL_HANDLE; timeline_ = VK_NULL_HANDLE;
        commandBuffers_.clear(); nextTimelineValue_ = 1; hasPendingWork_ = false;
    }
} // namespace Engine::RenderGraph
