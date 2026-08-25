#pragma once

/**
 * @file buffer.h
 * @brief Declares the Vulkan buffer wrapper used by the renderer.
 */

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace Engine {

/**
 * @brief Owns a Vulkan buffer and its Vulkan Memory Allocator allocation.
 *
 * The class provides helpers for device-local buffers initialized from CPU
 * data and host-visible buffers that can be updated from the CPU. A Buffer is
 * non-copyable and releases its resources in its destructor.
 */
class Buffer final {
public:
    /// Creates an empty buffer wrapper.
    Buffer() = default;

    /// Releases the owned Vulkan resources.
    ~Buffer();

    /// Copy construction is disabled because the object owns Vulkan handles.
    Buffer(const Buffer &) = delete;

    /// Copy assignment is disabled because the object owns Vulkan handles.
    Buffer &operator=(const Buffer &) = delete;

    /**
     * @brief Creates a device-local buffer and uploads initial data to it.
     * @param physicalDevice Vulkan physical device used to select memory.
     * @param device Logical Vulkan device that owns the buffer.
     * @param data Initial data to upload. Its size is specified by @p size.
     * @param size Buffer size in bytes.
     * @param usage Vulkan buffer usage flags.
     * @param commandPool Command pool used for the upload command buffer.
     * @param queue Queue used to submit the upload operation.
     * @param allocator VMA allocator used for the allocation.
     */
    void createDeviceLocal(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        const void *data,
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VkCommandPool commandPool,
        VkQueue queue,
        VmaAllocator allocator);

    /**
     * @brief Creates a host-visible buffer suitable for CPU updates.
     * @param physicalDevice Vulkan physical device used to select memory.
     * @param device Logical Vulkan device that owns the buffer.
     * @param size Buffer size in bytes.
     * @param usage Vulkan buffer usage flags.
     * @param allocator VMA allocator used for the allocation.
     */
    void createHostVisible(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VmaAllocator allocator);

    /**
     * @brief Copies data into the mapped host-visible allocation.
     * @param data Source data to copy.
     * @param size Number of bytes to copy.
     * @param offset Destination offset in bytes from the beginning of the buffer.
     */
    void update(const void* data, VkDeviceSize size, VkDeviceSize offset = 0) const;

    /// Releases the buffer, allocation and associated resources if present.
    void destroy() noexcept;

    /**
     * @brief Returns the Vulkan buffer handle.
     * @return The owned handle, or VK_NULL_HANDLE when no buffer was created.
     */
    [[nodiscard]] VkBuffer handle() const noexcept { return buffer_; }

private:
    struct CreateParameters {
        VkDevice device;
        VkDeviceSize size;
        VkBufferUsageFlags usage;
        VkMemoryPropertyFlags properties;
        VmaAllocator allocator;
    };

    VkDevice device_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkDeviceSize size_ = 0;
    void* mapped_ = nullptr;

    /**
     * @brief Creates a buffer with the requested memory properties.
     * @param physicalDevice Vulkan physical device used to select memory.
     * @param device Logical Vulkan device that owns the buffer.
     * @param size Buffer size in bytes.
     * @param usage Vulkan buffer usage flags.
     * @param properties Required Vulkan memory property flags.
     * @param allocator VMA allocator used for the allocation.
     */
    void create(const CreateParameters &parameters);

    /**
     * @brief Finds a compatible physical-device memory type.
     * @param physicalDevice Physical device whose memory properties are queried.
     * @param typeFilter Bit mask of candidate memory types.
     * @param properties Required memory property flags.
     * @return Index of a compatible memory type.
     */
    [[nodiscard]] static uint32_t findMemoryType(
        VkPhysicalDevice physicalDevice,
        uint32_t typeFilter,
        VkMemoryPropertyFlags properties);
};

} // namespace Engine
