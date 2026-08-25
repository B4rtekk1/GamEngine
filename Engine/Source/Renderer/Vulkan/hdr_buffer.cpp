#include "Engine/Renderer/Vulkan/hdr_buffer.h"

#include <stdexcept>

namespace Engine {
    namespace {
        uint32_t findMemoryType(const VkPhysicalDevice physicalDevice,
                                const uint32_t typeFilter,
                                const VkMemoryPropertyFlags properties) {
            VkPhysicalDeviceMemoryProperties memoryProperties{};
            vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
            for (uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index) {
                if ((typeFilter & (1U << index)) != 0 &&
                    (memoryProperties.memoryTypes[index].propertyFlags & properties) == properties) {
                    return index;
                }
            }
            throw std::runtime_error("Could not find memory for HDR buffer");
        }
    } // namespace

    HdrBuffer::~HdrBuffer() {
        destroy();
    }

    void HdrBuffer::create(const VkPhysicalDevice physicalDevice, const VkDevice device,
                           const VkExtent2D extent) {
        if (physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE ||
            extent.width == 0 || extent.height == 0) {
            throw std::invalid_argument("HDR buffer requires a device and non-zero extent");
        }

        VkFormatProperties formatProperties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice, Format, &formatProperties);
        constexpr VkFormatFeatureFlags required =
                static_cast<VkFormatFeatureFlags>(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) |
                static_cast<VkFormatFeatureFlags>(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
        if ((formatProperties.optimalTilingFeatures & required) != required) {
            throw std::runtime_error("GPU does not support RGBA16F as a sampled color attachment");
        }

        destroy();
        device_ = device;
        try {
            const VkImageCreateInfo imageInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = Format,
                .extent = {extent.width, extent.height, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                         static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_SAMPLED_BIT),
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            };
            if (vkCreateImage(device_, &imageInfo, nullptr, &image_) != VK_SUCCESS) {
                throw std::runtime_error("Could not create HDR image");
            }

            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(device_, image_, &requirements);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = findMemoryType(
                physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (vkAllocateMemory(device_, &allocation, nullptr, &memory_) != VK_SUCCESS ||
                vkBindImageMemory(device_, image_, memory_, 0) != VK_SUCCESS) {
                throw std::runtime_error("Could not allocate HDR image memory");
            }

            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = image_;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = Format;
            viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (vkCreateImageView(device_, &viewInfo, nullptr, &imageView_) != VK_SUCCESS) {
                throw std::runtime_error("Could not create HDR image view");
            }

            VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            samplerInfo.magFilter = VK_FILTER_LINEAR;
            samplerInfo.minFilter = VK_FILTER_LINEAR;
            samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.maxLod = 0.0F;
            if (vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_) != VK_SUCCESS) {
                throw std::runtime_error("Could not create HDR sampler");
            }
        } catch (...) {
            destroy();
            throw;
        }
    }

    void HdrBuffer::destroy() noexcept {
        if (device_ != VK_NULL_HANDLE) {
            if (sampler_ != VK_NULL_HANDLE) {
                vkDestroySampler(device_, sampler_, nullptr);
            }
            if (imageView_ != VK_NULL_HANDLE) { vkDestroyImageView(device_, imageView_, nullptr); }
            if (image_ != VK_NULL_HANDLE) { vkDestroyImage(device_, image_, nullptr); }
            if (memory_ != VK_NULL_HANDLE) { vkFreeMemory(device_, memory_, nullptr); }
        }
        sampler_ = VK_NULL_HANDLE;
        imageView_ = VK_NULL_HANDLE;
        image_ = VK_NULL_HANDLE;
        memory_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
    }
} // namespace Engine
