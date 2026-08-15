#include "Engine/Renderer/Textures/Texture2D.h"

#include "Engine/Renderer/Vulkan/buffer.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Engine {
    namespace {
        void transitionImage(
            VkCommandBuffer commandBuffer,
            VkImage image,
            std::uint32_t baseMipLevel,
            std::uint32_t levelCount,
            VkImageLayout oldLayout,
            VkImageLayout newLayout,
            VkAccessFlags sourceAccess,
            VkAccessFlags destinationAccess,
            VkPipelineStageFlags sourceStage,
            VkPipelineStageFlags destinationStage
        ) {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.oldLayout = oldLayout;
            barrier.newLayout = newLayout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMipLevel, levelCount, 0, 1};
            barrier.srcAccessMask = sourceAccess;
            barrier.dstAccessMask = destinationAccess;

            vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }
    }

    Texture2D::~Texture2D() {
        destroy();
    }

    Texture2D::Texture2D(Texture2D &&other) noexcept {
        *this = std::move(other);
    }

    Texture2D &Texture2D::operator=(Texture2D &&other) noexcept {
        if (this != &other) { return *this; }

        destroy();
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        image_ = std::exchange(other.image_, VK_NULL_HANDLE);
        memory_ = std::exchange(other.memory_, VK_NULL_HANDLE);
        imageView_ = std::exchange(other.imageView_, VK_NULL_HANDLE);
        sampler_ = std::exchange(other.sampler_, VK_NULL_HANDLE);
        format_ = std::exchange(other.format_, VK_FORMAT_UNDEFINED);
        width_ = std::exchange(other.width_, 0);
        height_ = std::exchange(other.height_, 0);
        mipLevels_ = std::exchange(other.mipLevels_, 0);
        return *this;
    }

    void Texture2D::create(
        const VkPhysicalDevice physicalDevice,
        const VkDevice device,
        const VkCommandPool commandPool,
        const VkQueue queue,
        const std::uint32_t width,
        const std::uint32_t height,
        const std::span<const std::uint8_t> rgbaPixels,
        const TextureColorSpace colorSpace,
        const bool generateMipmaps) {
        if (physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE ||
            commandPool == VK_NULL_HANDLE || queue == VK_NULL_HANDLE) {
            throw std::invalid_argument("Texture2D requires valid Vulkan handles");
        }
        if (width == 0 || height == 0) {
            throw std::invalid_argument("Texture2D dimensions cannot be zero");
        }
        if (width > std::numeric_limits<std::size_t>::max() / 4u / height) {
            throw std::invalid_argument("Texture2D dimensions are too large");
        }

        const std::size_t expectedSize = static_cast<std::size_t>(width) * height * 4u;
        if (rgbaPixels.size() != expectedSize) {
            throw std::invalid_argument("Texture2D requires exactly width * height RGBA8 pixels");
        }

        const VkFormat format = colorSpace == TextureColorSpace::SRGB
                                    ? VK_FORMAT_R8G8B8A8_SRGB
                                    : VK_FORMAT_R8G8B8A8_UNORM;
        const std::uint32_t mipLevels = generateMipmaps
                                            ? std::bit_width(std::max(width, height))
                                            : 1u;

        if (generateMipmaps && mipLevels > 1) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
            if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) == 0) {
                throw std::runtime_error("Texture2D format does not support linear mipmap blits");
            }
        }

        destroy();
        device_ = device;
        format_ = format;
        width_ = width;
        height_ = height;
        mipLevels_ = mipLevels;

        Buffer staging;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        try {
            staging.createHostVisible(
                physicalDevice, device_, static_cast<VkDeviceSize>(expectedSize),
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            staging.update(rgbaPixels.data(), static_cast<VkDeviceSize>(expectedSize));

            VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = format_;
            imageInfo.extent = {width_, height_, 1};
            imageInfo.mipLevels = mipLevels_;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            if (mipLevels_ > 1) {
                imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            }
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(device_, &imageInfo, nullptr, &image_) != VK_SUCCESS) {
                throw std::runtime_error("Could not create Texture2D image");
            }

            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(device_, image_, &requirements);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = findMemoryType(
                physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (vkAllocateMemory(device_, &allocation, nullptr, &memory_) != VK_SUCCESS) {
                throw std::runtime_error("Could not allocate Texture2D image memory");
            }
            if (vkBindImageMemory(device_, image_, memory_, 0) != VK_SUCCESS) {
                throw std::runtime_error("Could not bind Texture2D image memory");
            }

            VkCommandBufferAllocateInfo commandAllocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            commandAllocation.commandPool = commandPool;
            commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            commandAllocation.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(device_, &commandAllocation, &commandBuffer) != VK_SUCCESS) {
                throw std::runtime_error("Could not allocate Texture2D upload command buffer");
            }

            VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
                throw std::runtime_error("Could not begin Texture2D upload command buffer");
            }

            transitionImage(
                commandBuffer, image_, 0, mipLevels_,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {width_, height_, 1};
            vkCmdCopyBufferToImage(
                commandBuffer, staging.handle(), image_,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

            std::int32_t mipWidth = static_cast<std::int32_t>(width_);
            std::int32_t mipHeight = static_cast<std::int32_t>(height_);
            for (std::uint32_t level = 1; level < mipLevels_; ++level) {
                transitionImage(
                    commandBuffer, image_, level - 1, 1,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

                const std::int32_t nextWidth = std::max(mipWidth / 2, 1);
                const std::int32_t nextHeight = std::max(mipHeight / 2, 1);
                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
                blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
                blit.dstOffsets[1] = {nextWidth, nextHeight, 1};
                vkCmdBlitImage(
                    commandBuffer,
                    image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &blit, VK_FILTER_LINEAR);

                transitionImage(
                    commandBuffer, image_, level - 1, 1,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                mipWidth = nextWidth;
                mipHeight = nextHeight;
            }

            transitionImage(
                commandBuffer, image_, mipLevels_ - 1, 1,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

            if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
                throw std::runtime_error("Could not end Texture2D upload command buffer");
            }
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &commandBuffer;
            if (vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS ||
                vkQueueWaitIdle(queue) != VK_SUCCESS) {
                throw std::runtime_error("Could not upload Texture2D");
            }
            vkFreeCommandBuffers(device_, commandPool, 1, &commandBuffer);
            commandBuffer = VK_NULL_HANDLE;

            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = image_;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = format_;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels_, 0, 1};
            if (vkCreateImageView(device_, &view, nullptr, &imageView_) != VK_SUCCESS) {
                throw std::runtime_error("Could not create Texture2D image view");
            }

            VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            samplerInfo.magFilter = VK_FILTER_LINEAR;
            samplerInfo.minFilter = VK_FILTER_LINEAR;
            samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerInfo.minLod = 0.0f;
            samplerInfo.maxLod = static_cast<float>(mipLevels_ - 1);
            if (vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_) != VK_SUCCESS) {
                throw std::runtime_error("Could not create Texture2D sampler");
            }
        } catch (...) {
            if (commandBuffer != VK_NULL_HANDLE) {
                vkFreeCommandBuffers(device_, commandPool, 1, &commandBuffer);
            }
            destroy();
            throw;
        }
    }

    void Texture2D::destroy() noexcept {
        if (device_ != VK_NULL_HANDLE) {
            if (sampler_ != VK_NULL_HANDLE) {
                vkDestroySampler(device_, sampler_, nullptr);
            }
            if (imageView_ != VK_NULL_HANDLE) {
                vkDestroyImageView(device_, imageView_, nullptr);
            }
            if (image_ != VK_NULL_HANDLE) {
                vkDestroyImage(device_, image_, nullptr);
            }
            if (memory_ != VK_NULL_HANDLE) {
                vkFreeMemory(device_, memory_, nullptr);
            }
        }
        device_ = VK_NULL_HANDLE;
        image_ = VK_NULL_HANDLE;
        memory_ = VK_NULL_HANDLE;
        imageView_ = VK_NULL_HANDLE;
        sampler_ = VK_NULL_HANDLE;
        format_ = VK_FORMAT_UNDEFINED;
        width_ = 0;
        height_ = 0;
        mipLevels_ = 0;
    }

    std::uint32_t Texture2D::findMemoryType(
        const VkPhysicalDevice physicalDevice,
        const std::uint32_t typeFilter,
        const VkMemoryPropertyFlags properties) {
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
        for (std::uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index) {
            if ((typeFilter & (1u << index)) != 0 &&
                (memoryProperties.memoryTypes[index].propertyFlags & properties) == properties) {
                return index;
            }
        }
        throw std::runtime_error("Could not find Texture2D memory type");
    }
}
