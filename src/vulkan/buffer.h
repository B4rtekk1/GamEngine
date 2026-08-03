#pragma once

#include <vulkan/vulkan.h>

class Buffer final {
public:
    Buffer() = default;
    ~Buffer();

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    void createDeviceLocal(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        const void* data,
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VkCommandPool commandPool,
        VkQueue queue);
    void destroy() noexcept;

    [[nodiscard]] VkBuffer handle() const noexcept { return buffer_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;

    void create(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VkMemoryPropertyFlags properties);
    [[nodiscard]] uint32_t findMemoryType(
        VkPhysicalDevice physicalDevice,
        uint32_t typeFilter,
        VkMemoryPropertyFlags properties) const;
};
