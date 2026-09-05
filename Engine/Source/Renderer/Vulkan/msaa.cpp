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
        VkSampleCountFlagBits preferredSamples, VmaAllocator allocator) {
        physicalDevice_ = physicalDevice;
        device_ = device;
        allocator_ = allocator;
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

        for (const VkSampleCountFlagBits count: kSampleCountsDescending) {
            if (count <= preferredSamples &&
                (supported & static_cast<VkSampleCountFlags>(count)) != 0) {
                return count;
            }
        }

        return VK_SAMPLE_COUNT_1_BIT;
    }

    void MsaaResources::create(VkExtent2D extent, VkFormat colorFormat) {
        if (physicalDevice_ == VK_NULL_HANDLE || device_ == VK_NULL_HANDLE || allocator_ == VK_NULL_HANDLE) {
            throw std::runtime_error("MsaaResources not initialized");
        }

        destroy();

        // framebufferColorSampleCounts is only a device-wide upper bound.  A
        // specific format can support fewer samples for the exact image usage we
        // need here.  Creating a framebuffer from the broader limit is invalid;
        // several drivers defer reporting that error until vkQueueSubmit and may
        // report VK_ERROR_DEVICE_LOST.  Select the effective rate from the
        // format-specific capabilities before creating any attachment.
        VkImageFormatProperties formatProperties;
        const VkResult formatResult = vkGetPhysicalDeviceImageFormatProperties(
            physicalDevice_, colorFormat, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
            static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) |
            static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT),
            0, &formatProperties);
        if (formatResult != VK_SUCCESS) {
            throw std::runtime_error("GPU does not support the MSAA color image format");
        }
        for (const VkSampleCountFlagBits count: kSampleCountsDescending) {
            if (count <= sampleCount_ &&
                (formatProperties.sampleCounts & static_cast<VkSampleCountFlags>(count)) != 0) {
                sampleCount_ = count;
                break;
            }
        }

        if (!enabled()) {
            return;
        }

        // Vulkan requires zero-initialization for the unused fields, while its
        // sample-count enum has no zero enumerator.
        VkImageCreateInfo imageInfo{}; // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {extent.width, extent.height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = colorFormat;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) |
                          static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        imageInfo.samples = sampleCount_;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        try {
            VmaAllocationCreateInfo allocationInfo{};
            allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            if (vmaCreateImage(allocator_, &imageInfo, &allocationInfo, &colorImage_,
                               &colorImageAllocation_, nullptr) != VK_SUCCESS) {
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

    void MsaaResources::destroy() noexcept {
        if (device_ == VK_NULL_HANDLE) {
            return;
        }

        if (colorImageView_ != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, colorImageView_, nullptr);
            colorImageView_ = VK_NULL_HANDLE;
        }
        if (colorImage_ != VK_NULL_HANDLE) vmaDestroyImage(allocator_, colorImage_, colorImageAllocation_);
        colorImage_ = VK_NULL_HANDLE;
        colorImageAllocation_ = VK_NULL_HANDLE;
    }
} // namespace Engine
