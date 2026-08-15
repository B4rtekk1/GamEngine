#include "Engine/Renderer/Vulkan/buffer.h"

#include <cstring>
#include <stdexcept>

namespace Engine {

Buffer::~Buffer() {
    destroy();
}

void Buffer::createDeviceLocal(VkPhysicalDevice physicalDevice, VkDevice device, const void *data, VkDeviceSize size,
                               VkBufferUsageFlags usage, VkCommandPool commandPool, VkQueue queue) {
    if (data == nullptr || size == 0) {
        throw std::invalid_argument("Buffer upload requires non-empty data");
    }

    Buffer staging;
    staging.create(physicalDevice, device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    staging.update(data, size);

    create(physicalDevice, device, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

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

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence uploadFence = VK_NULL_HANDLE;
    if (vkCreateFence(device, &fenceInfo, nullptr, &uploadFence) != VK_SUCCESS) {
        vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
        throw std::runtime_error("Could not create buffer upload fence");
    }

    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    if (vkQueueSubmit(queue, 1, &submitInfo, uploadFence) != VK_SUCCESS ||
        vkWaitForFences(device, 1, &uploadFence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        vkDestroyFence(device, uploadFence, nullptr);
        vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
        throw std::runtime_error("Could not submit buffer copy command buffer");
    }

    vkDestroyFence(device, uploadFence, nullptr);
    vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
}

void Buffer::createHostVisible(const VkPhysicalDevice physicalDevice, const VkDevice device,
                               const VkDeviceSize size, const VkBufferUsageFlags usage) {
    if (size == 0) {
        throw std::invalid_argument("Host-visible buffer requires non-zero size");
    }
    create(physicalDevice, device, size, usage,
           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}

void Buffer::update(const void* data, const VkDeviceSize size, const VkDeviceSize offset) const {
    if (data == nullptr || size == 0 || offset > size_ || size > size_ - offset) {
        throw std::invalid_argument("Uniform buffer update is out of bounds");
    }
    if (mapped_ == nullptr) {
        throw std::runtime_error("Cannot update buffer without host-visible memory");
    }
    std::memcpy(static_cast<char*>(mapped_) + offset, data, static_cast<size_t>(size));
}

void Buffer::destroy() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        if (mapped_ != nullptr) {
            vkUnmapMemory(device_, memory_);
        }
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
    size_ = 0;
    mapped_ = nullptr;
}

void Buffer::create(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties) {
    destroy();
    device_ = device;
    size_ = size;

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
    if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 &&
        vkMapMemory(device_, memory_, 0, size_, 0, &mapped_) != VK_SUCCESS) {
        destroy();
        throw std::runtime_error("Could not persistently map host-visible buffer memory");
    }
}

uint32_t Buffer::findMemoryType(
    VkPhysicalDevice physicalDevice,
    uint32_t typeFilter,
    VkMemoryPropertyFlags properties) {
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

} // namespace Engine
