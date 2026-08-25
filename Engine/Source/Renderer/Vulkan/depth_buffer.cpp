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
            VK_FORMAT_D24_UNORM_S8_UINT,
        };

        for (VkFormat candidate: candidates) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(
                physicalDevice_,
                candidate,
                &properties);

            if ((properties.optimalTilingFeatures &
                 static_cast<VkFormatFeatureFlags>(VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) == 0) {
                continue;
            }

            VkImageFormatProperties imageProperties{};
            const VkResult result = vkGetPhysicalDeviceImageFormatProperties(
                physicalDevice_,
                candidate,
                VK_IMAGE_TYPE_2D,
                VK_IMAGE_TILING_OPTIMAL,
                static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) |
                static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_SAMPLED_BIT),
                0,
                &imageProperties);

            if (result == VK_SUCCESS &&
                (imageProperties.sampleCounts & static_cast<VkSampleCountFlags>(samples)) != 0) {
                return candidate;
            }
        }

        throw std::runtime_error(
            "GPU does not support the requested depth/stencil format for the selected MSAA sample count");
    }

    uint32_t DepthBuffer::findMemoryType(
        const MemoryTypeQuery &query) const {
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(
            physicalDevice_,
            &memoryProperties);

        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            const bool typeSupported = (query.typeFilter & (1U << i)) != 0;
            const bool propertiesSupported =
                    (memoryProperties.memoryTypes[i].propertyFlags & query.requiredProperties) ==
                    query.requiredProperties;

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
        VkSampleCountFlagBits samples,
        VkFormat requiredFormat) {
        if (physicalDevice_ == VK_NULL_HANDLE || device_ == VK_NULL_HANDLE) {
            throw std::logic_error("DepthBuffer has not been initialized");
        }
        if (extent.width == 0 || extent.height == 0) {
            throw std::invalid_argument("Depth buffer size cannot be zero");
        }
        destroy();
        format_ = requiredFormat == VK_FORMAT_UNDEFINED ? findSupportedFormat(samples) : requiredFormat;

        VkImageCreateInfo imageInfo{
            VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            nullptr,
            0,
            VK_IMAGE_TYPE_2D,
            format_,
            {extent.width, extent.height, 1},
            1,
            1,
            samples,
            VK_IMAGE_TILING_OPTIMAL,
            static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) |
            static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_SAMPLED_BIT),
            VK_SHARING_MODE_EXCLUSIVE,
            0,
            nullptr,
            VK_IMAGE_LAYOUT_UNDEFINED,
        };

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
                {
                    memoryRequirements.memoryTypeBits,
                    static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
                });

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

            VkImageAspectFlags aspectMask =
                    static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT);
            if (hasStencilComponent(format_)) {
                aspectMask |= static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_STENCIL_BIT);
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

            VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            samplerInfo.magFilter = VK_FILTER_NEAREST;
            samplerInfo.minFilter = VK_FILTER_NEAREST;
            samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.maxLod = 0.0F;
            if (vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_) != VK_SUCCESS) {
                throw std::runtime_error("Could not create depth sampler");
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
        if (sampler_ != VK_NULL_HANDLE) {
            vkDestroySampler(device_, sampler_, nullptr);
            sampler_ = VK_NULL_HANDLE;
        }
    }
} // namespace Engine
