#include "Engine/Renderer/Vulkan/depth_buffer.h"

#include <array>
#include <stdexcept>

namespace Engine {

DepthBuffer::~DepthBuffer() {
    destroy();
}

void DepthBuffer::initialize(
    VkPhysicalDevice physicalDevice,
    VkDevice device) {
    if (physicalDevice == VK_NULL_HANDLE) {
        throw std::invalid_argument("VkPhysicalDevice cannot be null");
    }
    if (device == VK_NULL_HANDLE) {
        throw std::invalid_argument("VkDevice cannot be null");
    }

    destroy();
    physicalDevice_ = physicalDevice;
    device_ = device;
    format_ = VK_FORMAT_UNDEFINED;
}

VkFormat DepthBuffer::findSupportedFormat(
    VkSampleCountFlagBits samples) const {
    constexpr std::array candidates = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT
    };

    for (VkFormat candidate : candidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(
            physicalDevice_,
            candidate,
            &properties);

        if ((properties.optimalTilingFeatures &
             VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0) {
            continue;
        }

        VkImageFormatProperties imageProperties{};
        const VkResult result = vkGetPhysicalDeviceImageFormatProperties(
            physicalDevice_,
            candidate,
            VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            0,
            &imageProperties);

        if (result == VK_SUCCESS &&
            (imageProperties.sampleCounts & samples) != 0) {
            return candidate;
        }
    }

    throw std::runtime_error(
        "GPU does not support the requested depth/stencil format for the selected MSAA sample count");
}

uint32_t DepthBuffer::findMemoryType(
    uint32_t typeFilter,
    VkMemoryPropertyFlags requiredProperties) const {
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(
        physicalDevice_,
        &memoryProperties);

    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        const bool typeSupported = (typeFilter & (1u << i)) != 0;
        const bool propertiesSupported =
            (memoryProperties.memoryTypes[i].propertyFlags & requiredProperties) ==
            requiredProperties;

        if (typeSupported && propertiesSupported) {
            return i;
        }
    }

    throw std::runtime_error(
        "GPU does not support the requested memory type for the depth buffer");
}

bool DepthBuffer::hasStencilComponent(VkFormat format) noexcept {
    return format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
           format == VK_FORMAT_D24_UNORM_S8_UINT;
}

void DepthBuffer::create(
    VkExtent2D extent,
    VkSampleCountFlagBits samples) {
    if (physicalDevice_ == VK_NULL_HANDLE || device_ == VK_NULL_HANDLE) {
        throw std::logic_error("DepthBuffer has not been initialized");
    }
    if (extent.width == 0 || extent.height == 0) {
        throw std::invalid_argument("Depth buffer size cannot be zero");
    }
    destroy();
    format_ = findSupportedFormat(samples);

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format_;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imageInfo.samples = samples;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device_, &imageInfo, nullptr, &image_) != VK_SUCCESS) {
        throw std::runtime_error("GPU does not support the requested image format for the depth buffer");
    }

    try {
        VkMemoryRequirements memoryRequirements{};
        vkGetImageMemoryRequirements(device_, image_, &memoryRequirements);

        VkMemoryAllocateInfo allocationInfo{};
        allocationInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocationInfo.allocationSize = memoryRequirements.size;
        allocationInfo.memoryTypeIndex = findMemoryType(
            memoryRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(
                device_,
                &allocationInfo,
                nullptr,
                &memory_) != VK_SUCCESS) {
            throw std::runtime_error(
                "GPU does not support the requested memory type for the depth buffer");
        }

        if (vkBindImageMemory(device_, image_, memory_, 0) != VK_SUCCESS) {
            throw std::runtime_error(
                "GPU does not support the requested memory type for the depth buffer");
        }

        VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (hasStencilComponent(format_)) {
            aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image_;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format_;
        viewInfo.subresourceRange.aspectMask = aspectMask;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(
                device_,
                &viewInfo,
                nullptr,
                &imageView_) != VK_SUCCESS) {
            throw std::runtime_error(
                "GPU does not support the requested image format for the depth buffer");
        }
    } catch (...) {
        destroy();
        throw;
    }
}

void DepthBuffer::destroy() noexcept {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }

    if (imageView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, imageView_, nullptr);
        imageView_ = VK_NULL_HANDLE;
    }
    if (image_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, image_, nullptr);
        image_ = VK_NULL_HANDLE;
    }
    if (memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, memory_, nullptr);
        memory_ = VK_NULL_HANDLE;
    }
}

} // namespace Engine
