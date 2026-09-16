#include "Engine/Renderer/Vulkan/shadow_map.h"

#include <array>
#include <stdexcept>

namespace Engine {
    namespace {
        VkFormat findDepthFormat(VkPhysicalDevice physicalDevice) {
            // The light-space depth range shifts with the clipmap origin.
            // D32 keeps comparisons stable while the camera moves; D16 can
            // visibly quantize the shadow edge as that origin is rebased.
            constexpr std::array formats{VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM};
            for (const VkFormat format: formats) {
                VkFormatProperties properties{};
                vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
                if ((properties.optimalTilingFeatures & static_cast<VkFormatFeatureFlags>(
                         VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0 &&
                    (properties.optimalTilingFeatures & static_cast<VkFormatFeatureFlags>(
                         VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) ==
                        static_cast<VkFormatFeatureFlags>(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                                         VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
                    return format;
                }
            }
            throw std::runtime_error("No depth format supports both shadow rendering and sampling");
        }
    }

    ShadowMap::~ShadowMap() { destroy(); }

    void ShadowMap::create(VkPhysicalDevice physicalDevice, VkDevice device, VmaAllocator allocator) {
        destroy();
        device_ = device;
        allocator_ = allocator;
        format_ = findDepthFormat(physicalDevice);
        try {
            VkImageCreateInfo imageInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = format_,
                .extent = {Resolution, Resolution, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) |
                         static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_SAMPLED_BIT),
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            };
            VmaAllocationCreateInfo allocationInfo{};
            allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            if (vmaCreateImage(allocator_, &imageInfo, &allocationInfo, &image_,
                               &allocation_, nullptr) != VK_SUCCESS) {
                throw std::runtime_error("Could not allocate shadow map memory");
            }

            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = image_;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = format_;
            view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            view.subresourceRange.levelCount = 1;
            view.subresourceRange.layerCount = 1;
            if (vkCreateImageView(device_, &view, nullptr, &imageView_) != VK_SUCCESS) {
                throw std::runtime_error(
                    "Could not create shadow map view");
            }

            VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            // The atlas contains unrelated virtual pages in adjacent physical
            // tiles.  Filtering must therefore happen in the shader after
            // resolving every virtual tap, never across physical tile edges.
            sampler.magFilter = VK_FILTER_NEAREST;
            sampler.minFilter = VK_FILTER_NEAREST;
            sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
            sampler.maxLod = 1.0F;
            sampler.compareEnable = VK_TRUE;
            sampler.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
            if (vkCreateSampler(device_, &sampler, nullptr, &sampler_) != VK_SUCCESS) {
                throw std::runtime_error(
                    "Could not create shadow map sampler");
            }
            // Hardware PCF is safe and much cheaper whenever the shader has
            // proven that the full 2x2 virtual footprint stays in this tile.
            // Format selection above guarantees this is supported for depth.
            sampler.magFilter = VK_FILTER_LINEAR;
            sampler.minFilter = VK_FILTER_LINEAR;
            if (vkCreateSampler(device_, &sampler, nullptr, &linearSampler_) != VK_SUCCESS) {
                throw std::runtime_error("Could not create linear shadow map sampler");
            }
            // PCSS needs the blocker depth itself, rather than a comparison
            // result. Keep the addressing/filtering identical to the compare
            // sampler so both paths resolve the exact same atlas texel.
            sampler.magFilter = VK_FILTER_NEAREST;
            sampler.minFilter = VK_FILTER_NEAREST;
            sampler.compareEnable = VK_FALSE;
            if (vkCreateSampler(device_, &sampler, nullptr, &depthSampler_) != VK_SUCCESS) {
                throw std::runtime_error("Could not create shadow depth sampler");
            }

        } catch (...) {
            destroy();
            throw;
        }
    }

    void ShadowMap::destroy() noexcept {
        if (device_ == VK_NULL_HANDLE) {
            return;
        }
        if (sampler_ != nullptr) {
            vkDestroySampler(device_, sampler_, nullptr);
        }
        if (linearSampler_ != nullptr) {
            vkDestroySampler(device_, linearSampler_, nullptr);
        }
        if (depthSampler_ != nullptr) {
            vkDestroySampler(device_, depthSampler_, nullptr);
        }
        if (imageView_ != nullptr) {
            vkDestroyImageView(device_, imageView_, nullptr);
        }
        if (image_ != nullptr) {
            vmaDestroyImage(allocator_, image_, allocation_);
        }
        sampler_ = VK_NULL_HANDLE;
        linearSampler_ = VK_NULL_HANDLE;
        depthSampler_ = VK_NULL_HANDLE;
        imageView_ = VK_NULL_HANDLE;
        image_ = VK_NULL_HANDLE;
        allocation_ = VK_NULL_HANDLE;
        allocator_ = VK_NULL_HANDLE;
        format_ = VK_FORMAT_UNDEFINED;
        initialized_ = false;
        device_ = VK_NULL_HANDLE;
    }
} // namespace Engine
