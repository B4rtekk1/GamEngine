#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstdint>
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
    void create(VkDevice device, VkQueue queue, uint32_t family, VmaAllocator allocator, VkDeviceSize bytes = 64ull * 1024 * 1024);
    void destroy() noexcept;
    /// Starts an explicit upload batch. Resources created while it is active
    /// append their copy commands instead of submitting independently.
    [[nodiscard]] Batch beginBatch();
    void begin();
    [[nodiscard]] Slice allocate(VkDeviceSize size, VkDeviceSize alignment = 16);
    void copyBuffer(VkBuffer destination, const void* data, VkDeviceSize size, VkDeviceSize destinationOffset = 0);
    [[nodiscard]] VkCommandBuffer commandBuffer() const noexcept { return commandBuffer_; }
    [[nodiscard]] UploadTicket submit();
    /// Ticket that will be signalled by the batch currently being recorded.
    [[nodiscard]] UploadTicket pendingTicket() const noexcept;
    [[nodiscard]] uint64_t completedValue() const noexcept;
    [[nodiscard]] VkSemaphore timeline() const noexcept { return timeline_; }
    [[nodiscard]] bool recording() const noexcept { return commandBuffer_ != VK_NULL_HANDLE; }
    static UploadContext* current() noexcept;
    static void setCurrent(UploadContext* context) noexcept;
private:
    struct Submitted { VkCommandBuffer commandBuffer{}; uint64_t value{}; };
    VkDevice device_{}; VkQueue queue_{}; VmaAllocator allocator_{}; VkBuffer staging_{}; VmaAllocation allocation_{}; void* mapped_{};
    VkDeviceSize capacity_{}; VkDeviceSize head_{}; VkCommandPool pool_{}; VkCommandBuffer commandBuffer_{}; VkSemaphore timeline_{}; uint64_t nextValue_{1}; std::vector<Submitted> submitted_;
    void reclaim() noexcept;
    void abort() noexcept;
    static UploadContext* current_;
};
}
