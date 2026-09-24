#include "Engine/Renderer/Vulkan/buffer.h"
#include <cassert>
#include "Engine/Renderer/Vulkan/upload_context.h"

#include <cstring>
#include <algorithm>
#include <array>
#include <string>
#include <stdexcept>

namespace Engine {
    Buffer::~Buffer() {
        destroy();
    }

    Buffer::Buffer(Buffer &&other) noexcept {
        *this = std::move(other);
    }

    Buffer &Buffer::operator=(Buffer &&other) noexcept {
        if (this == &other) { return *this;
}
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
        return *this;
    }

    void Buffer::createDeviceLocal([[maybe_unused]] VkPhysicalDevice physicalDevice, VkDevice device,
                                   const void *data, VkDeviceSize size,
                                   VkBufferUsageFlags usage, [[maybe_unused]] VkCommandPool commandPool,
                                   [[maybe_unused]] VkQueue queue,
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
        if (UploadContext *upload = UploadContext::current()) {
            const bool ownsBatch = !upload->recording();
            if (ownsBatch) { upload->begin();
}
            upload->copyBuffer(buffer_, data, size);
            readyTimeline_ = upload->pendingTicket().timelineValue;
            if (ownsBatch) { readyTimeline_ = upload->submit().timelineValue;
}
        } else {
            throw std::logic_error("Device-local buffer uploads require the central UploadContext");
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
        if (size == 0) { throw std::invalid_argument("Device-local buffer requires non-zero size");
}
        create({
            device, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, allocator, enableDeviceAddress,
        });
    }

    void Buffer::copyFromUploadRing(const VkBuffer source, const VkDeviceSize sourceOffset,
                                    const VkDeviceSize size, const VkDeviceSize destinationOffset) const {
        UploadContext *const upload = UploadContext::current();
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
        if (vmaFlushAllocation(allocator_, allocation_, offset, size) != VK_SUCCESS) {
            throw std::runtime_error("Could not flush host-visible buffer update");
        }
    }

    void Buffer::read(void *const destination, const VkDeviceSize size,
                      const VkDeviceSize offset) const {
        if (destination == nullptr || size == 0 || offset > size_ || size > size_ - offset) {
            throw std::invalid_argument("Buffer read out of bounds");
        }
        if (mapped_ == nullptr) {
            throw std::runtime_error("Cannot read buffer without host-visible memory");
        }
        if (vmaInvalidateAllocation(allocator_, allocation_, offset, size) != VK_SUCCESS) {
            throw std::runtime_error("Could not invalidate host-visible buffer read");
        }
        std::memcpy(destination, static_cast<const char *>(mapped_) + offset,
                    static_cast<size_t>(size));
    }

    void Buffer::uploadDeviceLocal(const void *data, const VkDeviceSize size,
                                   const VkDeviceSize offset, const VkCommandPool /*commandPool*/,
                                   const VkQueue /*queue*/) const {
        if (data == nullptr || size == 0 || offset > size_ || size > size_ - offset ||
            device_ == VK_NULL_HANDLE || allocator_ == VK_NULL_HANDLE) {
            throw std::invalid_argument("Device-local buffer update is out of bounds");
        }
        if (UploadContext *upload = UploadContext::current()) {
            const bool ownsBatch = !upload->recording();
            if (ownsBatch) { upload->begin();
}
            upload->copyBuffer(buffer_, data, size, offset);
            readyTimeline_ = upload->pendingTicket().timelineValue;
            if (ownsBatch) { readyTimeline_ = upload->submit().timelineValue;
}
            return;
        }
        throw std::logic_error("Device-local buffer uploads require the central UploadContext");
    }

    void Buffer::destroy() noexcept {
        // UploadContext submits copies independently of frame submissions.
        // A frame fence alone therefore cannot guarantee that this buffer is
        // no longer a transfer destination.
        if (readyTimeline_ != 0) {
            if (UploadContext *const upload = UploadContext::current()) {
                // Destroying a destination referenced by the active batch is
                // a lifetime violation. A non-submitted ticket after abort,
                // on the other hand, has no GPU work to wait for.
                assert(!upload->recording() ||
                       readyTimeline_ != upload->pendingTicket().timelineValue);
                if (upload->isSubmitted(readyTimeline_)) {
                    upload->wait(readyTimeline_);
                }
            }
            readyTimeline_ = 0;
        }
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
        if (!deviceAddressEnabled_ || device_ == VK_NULL_HANDLE || buffer_ == VK_NULL_HANDLE) { return 0;
}
        const VkBufferDeviceAddressInfo info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .buffer = buffer_,
        };
        return vkGetBufferDeviceAddress(device_, &info);
    }

    Buffer::MemoryInfo Buffer::memoryInfo(const VkPhysicalDevice physicalDevice) const noexcept {
        if (physicalDevice == VK_NULL_HANDLE || allocation_ == VK_NULL_HANDLE || size_ == 0) { return {};
}

        VmaAllocationInfo allocationInfo{};
        vmaGetAllocationInfo(allocator_, allocation_, &allocationInfo);
        VkPhysicalDeviceMemoryProperties deviceMemory{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &deviceMemory);
        if (allocationInfo.memoryType >= deviceMemory.memoryTypeCount) { return {};
}
        const VkMemoryType &memoryType = deviceMemory.memoryTypes[allocationInfo.memoryType];
        return {
            .heapIndex = memoryType.heapIndex, .properties = memoryType.propertyFlags,
            .bytes = allocationInfo.size,
        };
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
        if (deviceAddressEnabled_) { bufferInfo.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
}
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        std::array<uint32_t, 3> sharingFamilies{};
        if (const UploadContext *upload = UploadContext::current();
            upload != nullptr && upload->requiresConcurrentSharing()) {
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
        if ((parameters.properties & hostVisibleBit) == 0) {
            allocationInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        }
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
