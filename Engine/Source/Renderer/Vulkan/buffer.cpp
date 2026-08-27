#include "Engine/Renderer/Vulkan/buffer.h"

#include <cstring>
#include <string>
#include <stdexcept>

namespace Engine {
    Buffer::~Buffer() {
        destroy();
    }

    void Buffer::createDeviceLocal([[maybe_unused]] VkPhysicalDevice physicalDevice, VkDevice device,
                                   const void *data, VkDeviceSize size,
                                   VkBufferUsageFlags usage, VkCommandPool commandPool, VkQueue queue,
                                   VmaAllocator allocator) {
        if (data == nullptr || size == 0) {
            throw std::invalid_argument("Buffer upload requires non-empty data");
        }

        Buffer staging;
        staging.create({
            device, size,
            static_cast<VkBufferUsageFlags>(VK_BUFFER_USAGE_TRANSFER_SRC_BIT),
            static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) |
            static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
            allocator,
        });
        staging.update(data, size);

        create({
            device, size,
            usage | static_cast<VkBufferUsageFlags>(VK_BUFFER_USAGE_TRANSFER_DST_BIT),
            static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
            allocator,
        });

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
        const VkResult submitResult = vkQueueSubmit(queue, 1, &submitInfo, uploadFence);
        if (submitResult != VK_SUCCESS) {
            vkDestroyFence(device, uploadFence, nullptr);
            vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
            throw std::runtime_error("Could not submit buffer copy command buffer (vkQueueSubmit VkResult " +
                                     std::to_string(submitResult) + ")");
        }
        const VkResult waitResult = vkWaitForFences(device, 1, &uploadFence, VK_TRUE, UINT64_MAX);
        if (waitResult != VK_SUCCESS) {
            vkDestroyFence(device, uploadFence, nullptr);
            vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
            throw std::runtime_error("Buffer copy command buffer failed while waiting for fence (VkResult " +
                                     std::to_string(waitResult) + ")");
        }

        vkDestroyFence(device, uploadFence, nullptr);
        vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
    }

    void Buffer::createHostVisible([[maybe_unused]] const VkPhysicalDevice physicalDevice,
                                   const VkDevice device,
                                   const VkDeviceSize size, const VkBufferUsageFlags usage,
                                   VmaAllocator allocator) {
        if (size == 0) {
            throw std::invalid_argument("Host-visible buffer requires non-zero size");
        }
        create({
            device, size, usage,
            static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) |
            static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
            allocator,
        });
    }

    void Buffer::update(const void *data, const VkDeviceSize size, const VkDeviceSize offset) const {
        if (data == nullptr || size == 0 || offset > size_ || size > size_ - offset) {
            throw std::invalid_argument("Uniform buffer update is out of bounds");
        }
        if (mapped_ == nullptr) {
            throw std::runtime_error("Cannot update buffer without host-visible memory");
        }
        std::memcpy(static_cast<char *>(mapped_) + offset, data, static_cast<size_t>(size));
    }

    void Buffer::uploadDeviceLocal(const void* data, const VkDeviceSize size,
                                   const VkDeviceSize offset, const VkCommandPool commandPool,
                                   const VkQueue queue) const {
        if (data == nullptr || size == 0 || offset > size_ || size > size_ - offset ||
            device_ == VK_NULL_HANDLE || allocator_ == VK_NULL_HANDLE) {
            throw std::invalid_argument("Device-local buffer update is out of bounds");
        }
        Buffer staging;
        staging.create({
            device_, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            allocator_,
        });
        staging.update(data, size);

        VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocateInfo.commandPool = commandPool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(device_, &allocateInfo, &commandBuffer) != VK_SUCCESS) {
            throw std::runtime_error("Could not allocate device-local update command buffer");
        }
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
            vkFreeCommandBuffers(device_, commandPool, 1, &commandBuffer);
            throw std::runtime_error("Could not begin device-local buffer update");
        }
        const VkBufferCopy copy{.srcOffset = 0, .dstOffset = offset, .size = size};
        vkCmdCopyBuffer(commandBuffer, staging.buffer_, buffer_, 1, &copy);
        if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
            vkFreeCommandBuffers(device_, commandPool, 1, &commandBuffer);
            throw std::runtime_error("Could not finish device-local buffer update");
        }
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence = VK_NULL_HANDLE;
        if (vkCreateFence(device_, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
            vkFreeCommandBuffers(device_, commandPool, 1, &commandBuffer);
            throw std::runtime_error("Could not create device-local update fence");
        }
        VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        const VkResult submit = vkQueueSubmit(queue, 1, &submitInfo, fence);
        const VkResult wait = submit == VK_SUCCESS
                                  ? vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX)
                                  : submit;
        vkDestroyFence(device_, fence, nullptr);
        vkFreeCommandBuffers(device_, commandPool, 1, &commandBuffer);
        if (submit != VK_SUCCESS || wait != VK_SUCCESS) {
            throw std::runtime_error("Could not upload device-local buffer update");
        }
    }

    void Buffer::destroy() noexcept {
        if (device_ != VK_NULL_HANDLE) {
            if (buffer_ != VK_NULL_HANDLE) {
                vmaDestroyBuffer(allocator_, buffer_, allocation_);
            }
        }
        memory_ = VK_NULL_HANDLE;
        buffer_ = VK_NULL_HANDLE;
        allocation_ = VK_NULL_HANDLE;
        allocator_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
        size_ = 0;
        mapped_ = nullptr;
    }

    void Buffer::create(const CreateParameters &parameters) {
        destroy();
        if (parameters.allocator == VK_NULL_HANDLE) {
            throw std::invalid_argument("VMA allocator is null");
        }
        device_ = parameters.device;
        size_ = parameters.size;
        allocator_ = parameters.allocator;

        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = parameters.size;
        bufferInfo.usage = parameters.usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo allocationInfo{};
        constexpr VkMemoryPropertyFlags hostVisibleBit =
                static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        allocationInfo.usage = (parameters.properties & hostVisibleBit) != 0
                                   ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST
                                   : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if ((parameters.properties & hostVisibleBit) != 0) {
            allocationInfo.flags = static_cast<VmaAllocationCreateFlags>(
                                       VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT) |
                                   static_cast<VmaAllocationCreateFlags>(VMA_ALLOCATION_CREATE_MAPPED_BIT);
        }
        if (vmaCreateBuffer(parameters.allocator, &bufferInfo, &allocationInfo, &buffer_,
                            &allocation_, nullptr) != VK_SUCCESS) {
            destroy();
            throw std::runtime_error("Could not create Vulkan buffer with VMA");
        }
        if ((parameters.properties & hostVisibleBit) != 0) {
            VmaAllocationInfo info{};
            vmaGetAllocationInfo(parameters.allocator, allocation_, &info);
            mapped_ = info.pMappedData;
            memory_ = info.deviceMemory;
        }
    }

    uint32_t Buffer::findMemoryType(
        VkPhysicalDevice physicalDevice,
        uint32_t typeFilter,
        VkMemoryPropertyFlags properties) {
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            if ((typeFilter & (1U << i)) != 0 &&
                (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        throw std::runtime_error("Could not find a compatible Vulkan memory type");
    }
} // namespace Engine
