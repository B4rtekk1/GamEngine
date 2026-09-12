#include "Engine/Renderer/Vulkan/buffer.h"
#include "Engine/Renderer/Vulkan/upload_context.h"

#include <cstring>
#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <stdexcept>

namespace Engine {
    Buffer::~Buffer() {
        destroy();
    }

    Buffer::Buffer(Buffer&& other) noexcept {
        *this = std::move(other);
    }

    Buffer& Buffer::operator=(Buffer&& other) noexcept {
        if (this == &other) return *this;
        destroy();
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        buffer_ = std::exchange(other.buffer_, VK_NULL_HANDLE);
        memory_ = std::exchange(other.memory_, VK_NULL_HANDLE);
        allocation_ = std::exchange(other.allocation_, VK_NULL_HANDLE);
        allocator_ = std::exchange(other.allocator_, VK_NULL_HANDLE);
        size_ = std::exchange(other.size_, 0);
        mapped_ = std::exchange(other.mapped_, nullptr);
        deviceAddressEnabled_ = std::exchange(other.deviceAddressEnabled_, false);
        readyTimeline_ = std::exchange(other.readyTimeline_, 0);
        pendingUploads_ = std::move(other.pendingUploads_);
        return *this;
    }

    void Buffer::createDeviceLocal([[maybe_unused]] VkPhysicalDevice physicalDevice, VkDevice device,
                                   const void *data, VkDeviceSize size,
                                   VkBufferUsageFlags usage, VkCommandPool commandPool, VkQueue queue,
                                   VmaAllocator allocator) {
        if (data == nullptr || size == 0) {
            throw std::invalid_argument("Buffer upload requires non-empty data");
        }

        create({
            device, size,
            usage | static_cast<VkBufferUsageFlags>(VK_BUFFER_USAGE_TRANSFER_DST_BIT),
            static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
            allocator,
        });
        // Initial data follows the same non-blocking path as dynamic updates.
        // The staging allocation remains owned by this Buffer until its fence
        // signals, so its lifetime is valid without stalling the CPU.
        if (UploadContext* upload = UploadContext::current()) {
            const bool ownsBatch = !upload->recording();
            if (ownsBatch) upload->begin();
            upload->copyBuffer(buffer_, data, size);
            readyTimeline_ = upload->pendingTicket().timelineValue;
            if (ownsBatch) readyTimeline_ = upload->submit().timelineValue;
        } else {
            uploadDeviceLocal(data, size, 0, commandPool, queue);
        }
    }

    void Buffer::createHostVisible([[maybe_unused]] const VkPhysicalDevice physicalDevice,
                                   const VkDevice device,
                                   const VkDeviceSize size, const VkBufferUsageFlags usage,
                                   VmaAllocator allocator, const bool enableDeviceAddress) {
        if (size == 0) {
            throw std::invalid_argument("Host-visible buffer requires non-zero size");
        }
        create({
            device, size, usage,
            static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) |
            static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
            allocator, enableDeviceAddress,
        });
    }

    void Buffer::createDeviceLocalEmpty(const VkDevice device, const VkDeviceSize size,
                                        const VkBufferUsageFlags usage, VmaAllocator allocator,
                                        const bool enableDeviceAddress) {
        if (size == 0) throw std::invalid_argument("Device-local buffer requires non-zero size");
        create({device, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, allocator, enableDeviceAddress});
    }

    void Buffer::copyFromUploadRing(const VkBuffer source, const VkDeviceSize sourceOffset,
                                    const VkDeviceSize size, const VkDeviceSize destinationOffset) const {
        UploadContext* const upload = UploadContext::current();
        if (upload == nullptr || !upload->recording() || source == VK_NULL_HANDLE || size == 0 ||
            destinationOffset > size_ || size > size_ - destinationOffset) {
            throw std::invalid_argument("Invalid upload-ring buffer copy");
        }
        const VkBufferCopy copy{sourceOffset, destinationOffset, size};
        vkCmdCopyBuffer(upload->commandBuffer(), source, buffer_, 1, &copy);
        readyTimeline_ = upload->pendingTicket().timelineValue;
    }

    void Buffer::update(const void *data, const VkDeviceSize size, const VkDeviceSize offset) const {
        if (data == nullptr || size == 0 || offset > size_ || size > size_ - offset) {
            throw std::invalid_argument(
                "Buffer update out of bounds: requested=" + std::to_string(size) +
                ", offset=" + std::to_string(offset) +
                ", capacity=" + std::to_string(size_));
        }
        if (mapped_ == nullptr) {
            throw std::runtime_error("Cannot update buffer without host-visible memory");
        }
        std::memcpy(static_cast<char *>(mapped_) + offset, data, static_cast<size_t>(size));
    }

    void Buffer::read(void* const destination, const VkDeviceSize size,
                      const VkDeviceSize offset) const {
        if (destination == nullptr || size == 0 || offset > size_ || size > size_ - offset) {
            throw std::invalid_argument("Buffer read out of bounds");
        }
        if (mapped_ == nullptr) {
            throw std::runtime_error("Cannot read buffer without host-visible memory");
        }
        std::memcpy(destination, static_cast<const char*>(mapped_) + offset,
                    static_cast<size_t>(size));
    }

    void Buffer::uploadDeviceLocal(const void* data, const VkDeviceSize size,
                                   const VkDeviceSize offset, const VkCommandPool commandPool,
                                   const VkQueue queue) const {
        if (data == nullptr || size == 0 || offset > size_ || size > size_ - offset ||
            device_ == VK_NULL_HANDLE || allocator_ == VK_NULL_HANDLE) {
            throw std::invalid_argument("Device-local buffer update is out of bounds");
        }
        if (UploadContext* upload = UploadContext::current()) {
            const bool ownsBatch = !upload->recording();
            if (ownsBatch) upload->begin();
            upload->copyBuffer(buffer_, data, size, offset);
            readyTimeline_ = upload->pendingTicket().timelineValue;
            if (ownsBatch) readyTimeline_ = upload->submit().timelineValue;
            return;
        }
        reapCompletedUploads();

        auto staging = std::make_unique<Buffer>();
        staging->create({
            device_, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            allocator_,
        });
        staging->update(data, size);

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
        vkCmdCopyBuffer(commandBuffer, staging->buffer_, buffer_, 1, &copy);
        // The next graphics submission consumes this buffer as vertex data.
        // Queue order alone does not make transfer writes visible to the
        // vertex-input stage, which could otherwise render stale or partially
        // updated terrain vertices during a sculpt stroke.
        VkBufferMemoryBarrier visibilityBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        visibilityBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        visibilityBarrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        visibilityBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        visibilityBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        visibilityBarrier.buffer = buffer_;
        visibilityBarrier.offset = offset;
        visibilityBarrier.size = size;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, nullptr,
                             1, &visibilityBarrier, 0, nullptr);
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
        if (vkQueueSubmit(queue, 1, &submitInfo, fence) != VK_SUCCESS) {
            vkDestroyFence(device_, fence, nullptr);
            vkFreeCommandBuffers(device_, commandPool, 1, &commandBuffer);
            throw std::runtime_error("Could not upload device-local buffer update");
        }

        pendingUploads_.push_back({std::move(staging), commandPool, commandBuffer, fence});
    }

    void Buffer::reapCompletedUploads() const noexcept {
        if (device_ == VK_NULL_HANDLE) return;
        std::erase_if(pendingUploads_, [this](PendingUpload& upload) {
            if (vkGetFenceStatus(device_, upload.fence) != VK_SUCCESS) return false;
            vkDestroyFence(device_, upload.fence, nullptr);
            vkFreeCommandBuffers(device_, upload.commandPool, 1, &upload.commandBuffer);
            return true;
        });
    }

    void Buffer::finishPendingUploads() noexcept {
        for (PendingUpload& upload : pendingUploads_) {
            if (upload.fence != VK_NULL_HANDLE) {
                vkWaitForFences(device_, 1, &upload.fence, VK_TRUE, UINT64_MAX);
                vkDestroyFence(device_, upload.fence, nullptr);
            }
            if (upload.commandBuffer != VK_NULL_HANDLE) {
                vkFreeCommandBuffers(device_, upload.commandPool, 1, &upload.commandBuffer);
            }
        }
        pendingUploads_.clear();
    }

    void Buffer::destroy() noexcept {
        finishPendingUploads();
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
        deviceAddressEnabled_ = false;
    }

    VkDeviceAddress Buffer::deviceAddress() const noexcept {
        if (!deviceAddressEnabled_ || device_ == VK_NULL_HANDLE || buffer_ == VK_NULL_HANDLE) return 0;
        const VkBufferDeviceAddressInfo info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .buffer = buffer_,
        };
        return vkGetBufferDeviceAddress(device_, &info);
    }

    Buffer::MemoryInfo Buffer::memoryInfo(const VkPhysicalDevice physicalDevice) const noexcept {
        if (physicalDevice == VK_NULL_HANDLE || allocation_ == VK_NULL_HANDLE || size_ == 0) return {};

        VmaAllocationInfo allocationInfo{};
        vmaGetAllocationInfo(allocator_, allocation_, &allocationInfo);
        VkPhysicalDeviceMemoryProperties deviceMemory{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &deviceMemory);
        if (allocationInfo.memoryType >= deviceMemory.memoryTypeCount) return {};
        const VkMemoryType& memoryType = deviceMemory.memoryTypes[allocationInfo.memoryType];
        return {.heapIndex = memoryType.heapIndex, .properties = memoryType.propertyFlags,
                .bytes = allocationInfo.size};
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
        deviceAddressEnabled_ = parameters.enableDeviceAddress ||
            (parameters.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;
        bufferInfo.usage = parameters.usage;
        if (deviceAddressEnabled_) bufferInfo.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        std::array<uint32_t, 3> sharingFamilies{};
        if (const UploadContext* upload = UploadContext::current(); upload != nullptr && upload->requiresConcurrentSharing()) {
            sharingFamilies = upload->sharingFamilies();
            bufferInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
            bufferInfo.queueFamilyIndexCount = upload->sharingFamilyCount();
            bufferInfo.pQueueFamilyIndices = sharingFamilies.data();
        }
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
