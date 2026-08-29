#include "Engine/Renderer/Vulkan/shadow_map.h"

#include <array>
#include <stdexcept>

namespace Engine {
    namespace {
        uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits) {
            VkPhysicalDeviceMemoryProperties properties{};
            vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
            for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
                if ((typeBits & (1U << i)) != 0 &&
                    (properties.memoryTypes[i].propertyFlags & static_cast<VkMemoryPropertyFlags>(
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) != 0) {
                    return i;
                }
            }
            throw std::runtime_error("Could not find device-local memory for shadow map");
        }

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
                         VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) != 0) {
                    return format;
                }
            }
            throw std::runtime_error("No depth format supports both shadow rendering and sampling");
        }
    }

    ShadowMap::~ShadowMap() { destroy(); }

    void ShadowMap::create(VkPhysicalDevice physicalDevice, VkDevice device) {
        destroy();
        device_ = device;
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
            if (vkCreateImage(device_, &imageInfo, nullptr, &image_) != VK_SUCCESS) {
                throw std::runtime_error(
                    "Could not create shadow map image");
            }

            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(device_, image_, &requirements);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = findMemoryType(physicalDevice, requirements.memoryTypeBits);
            if (vkAllocateMemory(device_, &allocation, nullptr, &memory_) != VK_SUCCESS) {
                throw std::runtime_error(
                    "Could not allocate shadow map memory");
            }
            if (vkBindImageMemory(device_, image_, memory_, 0) != VK_SUCCESS) {
                throw std::runtime_error(
                    "Could not bind shadow map memory");
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
            sampler.magFilter = VK_FILTER_NEAREST;
            sampler.minFilter = VK_FILTER_NEAREST;
            sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
            sampler.maxLod = 1.0F;
            if (vkCreateSampler(device_, &sampler, nullptr, &sampler_) != VK_SUCCESS) {
                throw std::runtime_error(
                    "Could not create shadow map sampler");
            }

            VkAttachmentDescription depth{.samples = VK_SAMPLE_COUNT_1_BIT};
            depth.format = format_;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depth.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkAttachmentReference depthRef{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.pDepthStencilAttachment = &depthRef;
            std::array dependencies{VkSubpassDependency{}, VkSubpassDependency{}};
            dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
            dependencies[0].dstSubpass = 0;
            dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            dependencies[1].srcSubpass = 0;
            dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
            dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            pass.attachmentCount = 1;
            pass.pAttachments = &depth;
            pass.subpassCount = 1;
            pass.pSubpasses = &subpass;
            pass.dependencyCount = static_cast<uint32_t>(dependencies.size());
            pass.pDependencies = dependencies.data();
            if (vkCreateRenderPass(device_, &pass, nullptr, &renderPass_) != VK_SUCCESS) {
                throw std::runtime_error(
                    "Could not create shadow render pass");
            }
            VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            framebuffer.renderPass = renderPass_;
            framebuffer.attachmentCount = 1;
            framebuffer.pAttachments = &imageView_;
            framebuffer.width = Resolution;
            framebuffer.height = Resolution;
            framebuffer.layers = 1;
            if (vkCreateFramebuffer(device_, &framebuffer, nullptr, &framebuffer_) != VK_SUCCESS) {
                throw std::runtime_error("Could not create shadow framebuffer");
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
        if (framebuffer_ != nullptr) {
            vkDestroyFramebuffer(device_, framebuffer_, nullptr);
        }
        if (renderPass_ != nullptr) {
            vkDestroyRenderPass(device_, renderPass_, nullptr);
        }
        if (sampler_ != nullptr) {
            vkDestroySampler(device_, sampler_, nullptr);
        }
        if (imageView_ != nullptr) {
            vkDestroyImageView(device_, imageView_, nullptr);
        }
        if (image_ != nullptr) {
            vkDestroyImage(device_, image_, nullptr);
        }
        if (memory_ != nullptr) {
            vkFreeMemory(device_, memory_, nullptr);
        }
        framebuffer_ = VK_NULL_HANDLE;
        renderPass_ = VK_NULL_HANDLE;
        sampler_ = VK_NULL_HANDLE;
        imageView_ = VK_NULL_HANDLE;
        image_ = VK_NULL_HANDLE;
        memory_ = VK_NULL_HANDLE;
        format_ = VK_FORMAT_UNDEFINED;
        device_ = VK_NULL_HANDLE;
    }
} // namespace Engine
