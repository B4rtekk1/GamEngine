#include "Engine/Renderer/Vulkan/upload_context.h"
#include <cstring>
#include <cstddef>
#include <stdexcept>
#include <algorithm>
#include <utility>

namespace Engine {
    UploadContext *UploadContext::current_ = nullptr;
    UploadContext::~UploadContext() { destroy(); }
    UploadContext::Batch::Batch(UploadContext &context) : context_(&context) { context_->begin(); }

    UploadContext::Batch::~Batch() {
        if (context_ != nullptr) {
            context_->abort();
        }
    }

    UploadContext::Batch::Batch(Batch &&other) noexcept : context_(std::exchange(other.context_, nullptr)) {
    }

    UploadContext::Batch &UploadContext::Batch::operator=(Batch &&other) noexcept {
        if (this != &other) {
            if (context_ != nullptr) {
                context_->abort();
            }
            context_ = std::exchange(other.context_, nullptr);
        }
        return *this;
    }

    UploadTicket UploadContext::Batch::submit() {
        if (context_ == nullptr) {
            return {};
        }
        const UploadTicket ticket = context_->submit();
        context_ = nullptr;
        return ticket;
    }

    void UploadContext::setCurrent(UploadContext *context) noexcept { current_ = context; }
    UploadContext *UploadContext::current() noexcept { return current_; }

    void UploadContext::create(VkDevice device, VkQueue transferQueue, uint32_t transferFamily, VkQueue graphicsQueue,
                               uint32_t graphicsFamily, uint32_t computeFamily, VmaAllocator allocator,
                               VkDeviceSize bytes) {
        device_ = device;
        queue_ = transferQueue;
        graphicsQueue_ = graphicsQueue;
        queueFamily_ = transferFamily;
        graphicsFamily_ = graphicsFamily;
        computeFamily_ = computeFamily;
        allocator_ = allocator;
        capacity_ = bytes;
        splitQueues_ = queueFamily_ != graphicsFamily_;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        if (vmaCreateBuffer(allocator_, &info, &alloc, &staging_, &allocation_, nullptr) != VK_SUCCESS) {
            throw
                    std::runtime_error("Could not create upload staging ring");
        }
        VmaAllocationInfo details{};
        vmaGetAllocationInfo(allocator_, allocation_, &details);
        mapped_ = details.pMappedData;
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool.queueFamilyIndex = transferFamily;
        if (vkCreateCommandPool(device_, &pool, nullptr, &pool_) != VK_SUCCESS) {
            throw std::runtime_error(
                "Could not create upload command pool");
}
        if (splitQueues_) {
            pool.queueFamilyIndex = graphicsFamily_;
            if (vkCreateCommandPool(device_, &pool, nullptr, &graphicsPool_) != VK_SUCCESS) {
                throw std::runtime_error("Could not create upload graphics-finalizer command pool");
            }
        }
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type.initialValue = 0;
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        semaphore.pNext = &type;
        if (vkCreateSemaphore(device_, &semaphore, nullptr, &timeline_) != VK_SUCCESS) {
            throw std::runtime_error(
                "Could not create upload timeline semaphore");
        }
        if (splitQueues_ && vkCreateSemaphore(device_, &semaphore, nullptr, &copyTimeline_) != VK_SUCCESS) {
            throw std::runtime_error("Could not create upload copy timeline semaphore");
        }
    }

    bool UploadContext::overlapsLiveUploadRange(const VkDeviceSize begin,
                                                const VkDeviceSize end) const noexcept {
        const auto overlaps = [begin, end](const VkDeviceSize rangeBegin, const VkDeviceSize rangeEnd) {
            return begin < rangeEnd && rangeBegin < end;
        };
        if (hasActiveRange_ && overlaps(activeRangeBegin_, activeRangeEnd_)) return true;
        return std::any_of(retiredUploadRanges_.begin(), retiredUploadRanges_.end(),
                           [&overlaps](const RetiredUploadRange& range) {
                               return overlaps(range.begin, range.end);
                           });
    }

    void UploadContext::retireActiveUploadRange(const uint64_t timelineValue) noexcept {
        if (!hasActiveRange_) return;
        retiredUploadRanges_.push_back({activeRangeBegin_, activeRangeEnd_, timelineValue});
        hasActiveRange_ = false;
        tail_ = retiredUploadRanges_.front().begin;
    }

    void UploadContext::reclaim() noexcept {
        if (device_ == nullptr) {
            return;
        }
        uint64_t copyDone = completedValue();
        if (copyTimeline_ != nullptr) {
            vkGetSemaphoreCounterValue(device_, copyTimeline_, &copyDone);
        }
        const auto readyDone = completedValue();
        for (Submitted &submitted: submitted_) {
            if (submitted.copyCommandBuffer != VK_NULL_HANDLE && submitted.value <= copyDone) {
                vkFreeCommandBuffers(device_, pool_, 1, &submitted.copyCommandBuffer);
                submitted.copyCommandBuffer = VK_NULL_HANDLE;
            }
            if (submitted.graphicsCommandBuffer != VK_NULL_HANDLE && submitted.value <= readyDone) {
                vkFreeCommandBuffers(device_, graphicsPool_, 1, &submitted.graphicsCommandBuffer);
                submitted.graphicsCommandBuffer = VK_NULL_HANDLE;
            }
        }
        std::erase_if(submitted_, [](const Submitted &submitted) {
            return submitted.copyCommandBuffer == VK_NULL_HANDLE && submitted.graphicsCommandBuffer == VK_NULL_HANDLE;
        });
        std::erase_if(retiredUploadRanges_, [copyDone](const RetiredUploadRange& range) {
            return range.timelineValue <= copyDone;
        });
        tail_ = retiredUploadRanges_.empty() ? head_ : retiredUploadRanges_.front().begin;
    }

    void UploadContext::abort() noexcept {
        if (commandBuffer_ == VK_NULL_HANDLE) {
            return;
        }

        const VkCommandBuffer commandBuffer = std::exchange(commandBuffer_, VK_NULL_HANDLE);
        // A failed vkEndCommandBuffer or vkQueueSubmit2 leaves this buffer in a
        // non-recording state. Do not end it a second time during RAII cleanup.
        if (std::exchange(recording_, false)) {
            static_cast<void>(vkEndCommandBuffer(commandBuffer));
        }
        vkFreeCommandBuffers(device_, pool_, 1, &commandBuffer);
        if (graphicsCommandBuffer_ != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(device_, graphicsPool_, 1, &graphicsCommandBuffer_);
            graphicsCommandBuffer_ = VK_NULL_HANDLE;
        }
        // No command submitted this range, so it can immediately be reused.
        if (hasActiveRange_) {
            head_ = activeRangeBegin_;
            hasActiveRange_ = false;
        }
    }

    uint64_t UploadContext::completedValue() const noexcept {
        uint64_t value = 0;
        return (timeline_ != nullptr) && vkGetSemaphoreCounterValue(device_, timeline_, &value) == VK_SUCCESS
                   ? value
                   : 0;
    }

    void UploadContext::wait(const uint64_t value) const noexcept {
        if (value == 0 || timeline_ == VK_NULL_HANDLE || device_ == VK_NULL_HANDLE ||
            completedValue() >= value) {
            return;
        }

        VkSemaphoreWaitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &timeline_;
        waitInfo.pValues = &value;
        ++statistics_.waits;
        static_cast<void>(vkWaitSemaphores(device_, &waitInfo, UINT64_MAX));
    }

    UploadContext::Batch UploadContext::beginBatch() { return Batch{*this}; }

    void UploadContext::begin() {
        if (commandBuffer_ != VK_NULL_HANDLE) {
            throw std::logic_error("UploadContext command buffer is still active");
        }
        reclaim();
        VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        info.commandPool = pool_;
        info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device_, &info, &commandBuffer_) != VK_SUCCESS) {
            throw std::runtime_error("Could not allocate upload command buffer");
        }
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(commandBuffer_, &begin) != VK_SUCCESS) {
            vkFreeCommandBuffers(device_, pool_, 1, &commandBuffer_);
            commandBuffer_ = VK_NULL_HANDLE;
            throw std::runtime_error("Could not begin upload command buffer");
        }
        recording_ = true;
    }

    VkCommandBuffer UploadContext::graphicsCommandBuffer() {
        if (!recording_) {
            throw std::logic_error("UploadContext graphics finalizer requested outside an upload batch");
        }
        if (!splitQueues_) {
            return commandBuffer_;
        }
        if (graphicsCommandBuffer_ != VK_NULL_HANDLE) {
            return graphicsCommandBuffer_;
        }
        VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        info.commandPool = graphicsPool_;
        info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device_, &info, &graphicsCommandBuffer_) != VK_SUCCESS) {
            throw std::runtime_error("Could not allocate upload graphics-finalizer command buffer");
        }
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(graphicsCommandBuffer_, &begin) != VK_SUCCESS) {
            vkFreeCommandBuffers(device_, graphicsPool_, 1, &graphicsCommandBuffer_);
            graphicsCommandBuffer_ = VK_NULL_HANDLE;
            throw std::runtime_error("Could not begin upload graphics-finalizer command buffer");
        }
        return graphicsCommandBuffer_;
    }

    UploadContext::Slice UploadContext::allocate(VkDeviceSize size, VkDeviceSize alignment) {
        if (!recording_ || size > capacity_) {
            throw std::runtime_error("Invalid upload-ring allocation");
        }
        for (;;) {
            reclaim();
            VkDeviceSize offset = (head_ + alignment - 1) & ~(alignment - 1);
            if (offset + size <= capacity_ && !overlapsLiveUploadRange(offset, offset + size)) {
                const VkDeviceSize rangeBegin = hasActiveRange_ ? activeRangeBegin_ : head_;
                head_ = offset + size;
                activeRangeBegin_ = rangeBegin;
                activeRangeEnd_ = head_;
                hasActiveRange_ = true;
                statistics_.bytesUploaded += size;
                return {staging_, offset, static_cast<char *>(mapped_) + offset};
            }

            // A submitted range never wraps physically. Once the current
            // batch has data, submit it before trying the start of the ring.
            if (hasActiveRange_) {
                ++statistics_.forcedSubmits;
                static_cast<void>(submit());
                begin();
                continue;
            }

            // The beginning may already have been retired even while the end
            // is still in flight. Reuse it without waiting whenever possible.
            if (size <= capacity_ && !overlapsLiveUploadRange(0, size)) {
                ++statistics_.ringWraps;
                head_ = size;
                activeRangeBegin_ = 0;
                activeRangeEnd_ = head_;
                hasActiveRange_ = true;
                statistics_.bytesUploaded += size;
                return {staging_, 0, mapped_};
            }

            // The ring is genuinely full. Wait only for the oldest blocking
            // submission, reclaim its range, and retry the allocation.
            if (retiredUploadRanges_.empty()) {
                throw std::runtime_error("Upload ring has no reclaimable range");
            }
            wait(retiredUploadRanges_.front().timelineValue);
        }
    }

    void UploadContext::copyBuffer(VkBuffer dst, const void *data, VkDeviceSize size, VkDeviceSize dstOffset) {
        const auto *source = static_cast<const std::byte *>(data);
        while (size != 0) {
            const VkDeviceSize chunkSize = std::min(size, capacity_);
            const auto slice = allocate(chunkSize);
            std::memcpy(slice.mapped, source, static_cast<size_t>(chunkSize));
            const VkBufferCopy copy{slice.offset, dstOffset, chunkSize};
            vkCmdCopyBuffer(commandBuffer_, staging_, dst, 1, &copy);
            source += chunkSize;
            dstOffset += chunkSize;
            size -= chunkSize;
        }
    }

    UploadTicket UploadContext::pendingTicket() const noexcept {
        return recording_ ? UploadTicket{nextValue_} : UploadTicket{};
    }

    UploadTicket UploadContext::submit() {
        if (commandBuffer_ == VK_NULL_HANDLE) {
            return {};
        }
        if (!recording_) {
            throw std::logic_error("Upload command buffer is not recording");
        }

        recording_ = false;
        if (vkEndCommandBuffer(commandBuffer_) != VK_SUCCESS) {
            throw std::runtime_error("Could not end upload command buffer");
        }
        const uint64_t value = nextValue_;
        const bool hasFinalizer = graphicsCommandBuffer_ != VK_NULL_HANDLE;
        if (hasFinalizer && vkEndCommandBuffer(graphicsCommandBuffer_) != VK_SUCCESS) {
            throw std::runtime_error("Could not end upload graphics-finalizer command buffer");
        }
        VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        command.commandBuffer = commandBuffer_;
        VkSemaphoreSubmitInfo copySignal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        copySignal.semaphore = copyTimeline_;
        copySignal.value = value;
        copySignal.stageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        VkSemaphoreSubmitInfo readySignal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        readySignal.semaphore = timeline_;
        readySignal.value = value;
        readySignal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSemaphoreSubmitInfo previousReady{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        previousReady.semaphore = timeline_;
        previousReady.value = value - 1;
        previousReady.stageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &command;
        // A later copy submission must not signal a greater ready-timeline value
        // before the preceding graphics finalizer signals its value on another
        // queue.  This preserves the required global monotonic signal order.
        if (splitQueues_ && value > 1) {
            submit.waitSemaphoreInfoCount = 1;
            submit.pWaitSemaphoreInfos = &previousReady;
        }
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = hasFinalizer ? &copySignal : &readySignal;
        if (vkQueueSubmit2(queue_, 1, &submit,VK_NULL_HANDLE) != VK_SUCCESS) {
            throw std::runtime_error(
                "Could not submit upload batch");
        }
        if (hasFinalizer) {
            VkSemaphoreSubmitInfo copyWait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
            copyWait.semaphore = copyTimeline_;
            copyWait.value = value;
            copyWait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            VkCommandBufferSubmitInfo graphicsCommand{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
            graphicsCommand.commandBuffer = graphicsCommandBuffer_;
            VkSubmitInfo2 graphicsSubmit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
            graphicsSubmit.waitSemaphoreInfoCount = 1;
            graphicsSubmit.pWaitSemaphoreInfos = &copyWait;
            graphicsSubmit.commandBufferInfoCount = 1;
            graphicsSubmit.pCommandBufferInfos = &graphicsCommand;
            graphicsSubmit.signalSemaphoreInfoCount = 1;
            graphicsSubmit.pSignalSemaphoreInfos = &readySignal;
            if (vkQueueSubmit2(graphicsQueue_, 1, &graphicsSubmit, VK_NULL_HANDLE) != VK_SUCCESS) {
                throw std::runtime_error("Could not submit upload graphics finalizer");
            }
        }
        submitted_.push_back({commandBuffer_, hasFinalizer ? graphicsCommandBuffer_ : VK_NULL_HANDLE, value});
        retireActiveUploadRange(value);
        commandBuffer_ = VK_NULL_HANDLE;
        graphicsCommandBuffer_ = VK_NULL_HANDLE;
        ++nextValue_;
        return {value};
    }

    void UploadContext::destroy() noexcept {
        if (device_ == nullptr) {
            return;
        }
        if (commandBuffer_ != nullptr) { abort(); }
        if (graphicsCommandBuffer_ != nullptr) {
            vkFreeCommandBuffers(device_, graphicsPool_, 1, &graphicsCommandBuffer_);
            graphicsCommandBuffer_ = VK_NULL_HANDLE;
        }
        if ((timeline_ != nullptr) && nextValue_ > 1) {
            VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            const uint64_t v = nextValue_ - 1;
            wait.semaphoreCount = 1;
            wait.pSemaphores = &timeline_;
            wait.pValues = &v;
            vkWaitSemaphores(device_, &wait,UINT64_MAX);
        }
        for (auto &s: submitted_) {
            if (s.copyCommandBuffer != nullptr) {
                vkFreeCommandBuffers(device_, pool_, 1, &s.copyCommandBuffer);
            }
            if (s.graphicsCommandBuffer != nullptr) {
                vkFreeCommandBuffers(device_, graphicsPool_, 1, &s.graphicsCommandBuffer);
            }
        }
        submitted_.clear();
        if (copyTimeline_ != nullptr) {
            vkDestroySemaphore(device_, copyTimeline_, nullptr);
        }
        if (timeline_ != nullptr) {
            vkDestroySemaphore(device_, timeline_, nullptr);
        }
        if (graphicsPool_ != nullptr) {
            vkDestroyCommandPool(device_, graphicsPool_, nullptr);
        }
        if (pool_ != nullptr) {
            vkDestroyCommandPool(device_, pool_, nullptr);
        }
        if (staging_ != nullptr) {
            vmaDestroyBuffer(allocator_, staging_, allocation_);
        }
        if (current_ == this) {
            current_ = nullptr;
        }
        device_ = VK_NULL_HANDLE;
        queue_ = graphicsQueue_ = VK_NULL_HANDLE;
        queueFamily_ = graphicsFamily_ = computeFamily_ = 0;
        allocator_ = VK_NULL_HANDLE;
        staging_ = VK_NULL_HANDLE;
        allocation_ = VK_NULL_HANDLE;
        mapped_ = nullptr;
        capacity_ = head_ = tail_ = activeRangeBegin_ = activeRangeEnd_ = 0;
        hasActiveRange_ = false;
        pool_ = graphicsPool_ = VK_NULL_HANDLE;
        timeline_ = copyTimeline_ = VK_NULL_HANDLE;
        nextValue_ = 1;
        splitQueues_ = false;
        recording_ = false;
        statistics_ = {};
        retiredUploadRanges_.clear();
    }
}
