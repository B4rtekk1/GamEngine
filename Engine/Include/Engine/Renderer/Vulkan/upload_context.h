#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstdint>
#include <vector>

namespace Engine {
class UploadContext final {
public:
    struct Slice { VkBuffer buffer{}; VkDeviceSize offset{}; void* mapped{}; };
    UploadContext() = default; ~UploadContext();
    UploadContext(const UploadContext&) = delete; UploadContext& operator=(const UploadContext&) = delete;
    void create(VkDevice device, VkQueue queue, uint32_t family, VmaAllocator allocator, VkDeviceSize bytes = 64ull * 1024 * 1024);
    void destroy() noexcept;
    void begin();
    [[nodiscard]] Slice allocate(VkDeviceSize size, VkDeviceSize alignment = 16);
    void copyBuffer(VkBuffer destination, const void* data, VkDeviceSize size, VkDeviceSize destinationOffset = 0);
    [[nodiscard]] VkCommandBuffer commandBuffer() const noexcept { return commandBuffer_; }
    [[nodiscard]] uint64_t submit();
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
    static UploadContext* current_;
};
}
