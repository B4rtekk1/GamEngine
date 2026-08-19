#include "Engine/Renderer/Vulkan/msaa.h"

#include <stdexcept>

namespace Engine {

namespace {

constexpr VkSampleCountFlagBits kSampleCountsDescending[] = {
    VK_SAMPLE_COUNT_64_BIT,
    VK_SAMPLE_COUNT_32_BIT,
    VK_SAMPLE_COUNT_16_BIT,
    VK_SAMPLE_COUNT_8_BIT,
    VK_SAMPLE_COUNT_4_BIT,
    VK_SAMPLE_COUNT_2_BIT,
    VK_SAMPLE_COUNT_1_BIT,
};

} // namespace

void MsaaResources::initialize(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkSampleCountFlagBits preferredSamples) {
    physicalDevice_ = physicalDevice;
    device_ = device;
    sampleCount_ = chooseSampleCount(physicalDevice, preferredSamples);
}

VkSampleCountFlagBits MsaaResources::chooseSampleCount(
    VkPhysicalDevice physicalDevice,
    VkSampleCountFlagBits preferredSamples) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);

    // Every attachment in the forward render pass must support the same
    // sample count.  Checking only the color limit can select 4x on GPUs
    // whose depth attachment supports only 1x/2x.  The resulting framebuffer
    // is invalid and, on some drivers, the first submission loses the device;
    // the next renderer reload then reports VK_ERROR_DEVICE_LOST from
    // vkCreateDevice.
    const VkSampleCountFlags supported =
        properties.limits.framebufferColorSampleCounts &
        properties.limits.framebufferDepthSampleCounts;

    for (const VkSampleCountFlagBits count : kSampleCountsDescending) {
        if (count <= preferredSamples && (supported & count) != 0) {
            return count;
        }
    }

    return VK_SAMPLE_COUNT_1_BIT;
}

uint32_t MsaaResources::findMemoryType(
    uint32_t typeFilter,
    VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties);

    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        const bool allowed = (typeFilter & (1u << i)) != 0;
        const bool hasProperties =
            (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties;
        if (allowed && hasProperties) {
            return i;
        }
    }

    throw std::runtime_error("GPU does not support the requested memory type for MSAA");
}

void MsaaResources::create(VkExtent2D extent, VkFormat colorFormat) {
    if (physicalDevice_ == VK_NULL_HANDLE || device_ == VK_NULL_HANDLE) {
        throw std::runtime_error("MsaaResources not initialized");
    }

    destroy();

    if (!enabled()) {
        return;
    }

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = colorFormat;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    imageInfo.samples = sampleCount_;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device_, &imageInfo, nullptr, &colorImage_) != VK_SUCCESS) {
        throw std::runtime_error("GPU does not support the requested image format for MSAA");
    }

    try {
        VkMemoryRequirements memoryRequirements{};
        vkGetImageMemoryRequirements(device_, colorImage_, &memoryRequirements);

        VkMemoryAllocateInfo allocationInfo{};
        allocationInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocationInfo.allocationSize = memoryRequirements.size;
        allocationInfo.memoryTypeIndex = findMemoryType(
            memoryRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device_, &allocationInfo, nullptr, &colorImageMemory_) != VK_SUCCESS) {
            throw std::runtime_error("GPU does not support the requested memory type for MSAA");
        }

        if (vkBindImageMemory(device_, colorImage_, colorImageMemory_, 0) != VK_SUCCESS) {
            throw std::runtime_error("GPU does not support the requested memory type for MSAA");
        }

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = colorImage_;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = colorFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device_, &viewInfo, nullptr, &colorImageView_) != VK_SUCCESS) {
            throw std::runtime_error("GPU does not support the requested image format for MSAA");
        }
    } catch (...) {
        destroy();
        throw;
    }
}

void MsaaResources::destroy() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }

    if (colorImageView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, colorImageView_, nullptr);
        colorImageView_ = VK_NULL_HANDLE;
    }
    if (colorImage_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, colorImage_, nullptr);
        colorImage_ = VK_NULL_HANDLE;
    }
    if (colorImageMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, colorImageMemory_, nullptr);
        colorImageMemory_ = VK_NULL_HANDLE;
    }
}

} // namespace Engine
