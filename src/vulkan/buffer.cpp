#include "buffer.h"

#include <cstring>
#include <stdexcept>

Buffer::~Buffer() {
    destroy();
}

void Buffer::createDeviceLocal(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    const void* data,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkCommandPool commandPool,
    VkQueue queue) {
    if (data == nullptr || size == 0) {
        throw std::invalid_argument("Buffer upload requires non-empty data");
    }

    Buffer staging;
    staging.create(
        physicalDevice, device, size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    void* mapped = nullptr;
    if (vkMapMemory(device, staging.memory_, 0, size, 0, &mapped) != VK_SUCCESS) {
        throw std::runtime_error("Could not map staging buffer memory");
    }
    std::memcpy(mapped, data, static_cast<size_t>(size));
    vkUnmapMemory(device, staging.memory_);

    create(
        physicalDevice, device, size,
        usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocateInfo.commandPool = commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device, &allocateInfo, &commandBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Could not allocate buffer copy command buffer");
    }

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
        throw std::runtime_error("Could not begin buffer copy command buffer");
    }

    VkBufferCopy copyRegion{.size = size};
    vkCmdCopyBuffer(commandBuffer, staging.buffer_, buffer_, 1, &copyRegion);

    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
        throw std::runtime_error("Could not end buffer copy command buffer");
    }

    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    if (vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS ||
        vkQueueWaitIdle(queue) != VK_SUCCESS) {
        vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
        throw std::runtime_error("Could not submit buffer copy command buffer");
    }

    vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
}

void Buffer::destroy() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        if (buffer_ != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, buffer_, nullptr);
        }
        if (memory_ != VK_NULL_HANDLE) {
            vkFreeMemory(device_, memory_, nullptr);
        }
    }
    memory_ = VK_NULL_HANDLE;
    buffer_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}

void Buffer::create(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties) {
    destroy();
    device_ = device;

    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer_) != VK_SUCCESS) {
        destroy();
        throw std::runtime_error("Could not create Vulkan buffer");
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer_, &requirements);

    VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = findMemoryType(physicalDevice, requirements.memoryTypeBits, properties);
    if (vkAllocateMemory(device_, &allocateInfo, nullptr, &memory_) != VK_SUCCESS) {
        destroy();
        throw std::runtime_error("Could not allocate Vulkan buffer memory");
    }
    if (vkBindBufferMemory(device_, buffer_, memory_, 0) != VK_SUCCESS) {
        destroy();
        throw std::runtime_error("Could not bind Vulkan buffer memory");
    }
}

uint32_t Buffer::findMemoryType(
    VkPhysicalDevice physicalDevice,
    uint32_t typeFilter,
    VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) != 0 &&
            (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    throw std::runtime_error("Could not find a compatible Vulkan memory type");
}
