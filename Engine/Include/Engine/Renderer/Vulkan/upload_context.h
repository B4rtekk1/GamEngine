#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstdint>
#include <array>
#include <vector>

namespace Engine {
struct UploadTicket final {
    uint64_t timelineValue{};

    [[nodiscard]] explicit operator bool() const noexcept { return timelineValue != 0; }
};

class UploadContext final {
public:
    struct Slice { VkBuffer buffer{}; VkDeviceSize offset{}; void* mapped{}; };
    class Batch final {
    public:
        explicit Batch(UploadContext& context);
        ~Batch();
        Batch(const Batch&) = delete;
        Batch& operator=(const Batch&) = delete;
        Batch(Batch&& other) noexcept;
        Batch& operator=(Batch&& other) noexcept;

        /// Ends and submits all uploads recorded since this batch was created.
        [[nodiscard]] UploadTicket submit();

    private:
        UploadContext* context_{};
    };

    UploadContext() = default; ~UploadContext();
    UploadContext(const UploadContext&) = delete; UploadContext& operator=(const UploadContext&) = delete;
    void create(VkDevice device, VkQueue transferQueue, uint32_t transferFamily, VkQueue graphicsQueue,
                uint32_t graphicsFamily, uint32_t computeFamily,
                VmaAllocator allocator, VkDeviceSize bytes = 32ull * 1024 * 1024);
    void destroy() noexcept;
    /// Starts an explicit upload batch. Resources created while it is active
    /// append their copy commands instead of submitting independently.
    [[nodiscard]] Batch beginBatch();
    void begin();
    [[nodiscard]] Slice allocate(VkDeviceSize size, VkDeviceSize alignment = 16);
    void copyBuffer(VkBuffer destination, const void* data, VkDeviceSize size, VkDeviceSize destinationOffset = 0);
    /// Records work which must execute on the graphics queue (mip blits and
    /// shader-visible image transitions).  On a shared queue this is the copy
    /// command buffer, so callers do not need a special fallback path.
    [[nodiscard]] VkCommandBuffer graphicsCommandBuffer();
    [[nodiscard]] VkCommandBuffer commandBuffer() const noexcept { return commandBuffer_; }
    [[nodiscard]] UploadTicket submit();
    /// Ticket that will be signalled by the batch currently being recorded.
    [[nodiscard]] UploadTicket pendingTicket() const noexcept;
    [[nodiscard]] uint64_t completedValue() const noexcept;
    /// Waits until the upload timeline has reached @p value.  This is used
    /// when an asynchronously-uploaded destination resource is retired.
    void wait(uint64_t value) const noexcept;
    [[nodiscard]] uint64_t lastSubmittedValue() const noexcept { return nextValue_ - 1; }
    [[nodiscard]] VkSemaphore timeline() const noexcept { return timeline_; }
    [[nodiscard]] bool requiresConcurrentSharing() const noexcept { return sharingFamilyCount() > 1; }
    [[nodiscard]] std::array<uint32_t, 3> sharingFamilies() const noexcept {
        std::array<uint32_t, 3> families{queueFamily_, 0, 0};
        uint32_t count = 1;
        if (graphicsFamily_ != queueFamily_) families[count++] = graphicsFamily_;
        if (computeFamily_ != queueFamily_ && computeFamily_ != graphicsFamily_) families[count] = computeFamily_;
        return families;
    }
    [[nodiscard]] uint32_t sharingFamilyCount() const noexcept {
        uint32_t count = queueFamily_ == graphicsFamily_ ? 1u : 2u;
        if (computeFamily_ != queueFamily_ && computeFamily_ != graphicsFamily_) ++count;
        return count;
    }
    [[nodiscard]] bool recording() const noexcept { return recording_; }
    [[nodiscard]] VkDeviceSize capacity() const noexcept { return capacity_; }
    static UploadContext* current() noexcept;
    static void setCurrent(UploadContext* context) noexcept;
private:
    struct Submitted { VkCommandBuffer copyCommandBuffer{}; VkCommandBuffer graphicsCommandBuffer{}; uint64_t value{}; };
    VkDevice device_{}; VkQueue queue_{}; VkQueue graphicsQueue_{}; uint32_t queueFamily_{}; uint32_t graphicsFamily_{}; uint32_t computeFamily_{}; VmaAllocator allocator_{}; VkBuffer staging_{}; VmaAllocation allocation_{}; void* mapped_{};
    VkDeviceSize capacity_{}; VkDeviceSize head_{}; VkCommandPool pool_{};
    VkCommandPool graphicsPool_{}; VkCommandBuffer commandBuffer_{}; VkCommandBuffer graphicsCommandBuffer_{};
    VkSemaphore timeline_{}; VkSemaphore copyTimeline_{}; uint64_t nextValue_{1};
    bool splitQueues_{};
    bool recording_{};
    std::vector<Submitted> submitted_;
    void reclaim() noexcept;
    void abort() noexcept;
    static UploadContext* current_;
};
}
