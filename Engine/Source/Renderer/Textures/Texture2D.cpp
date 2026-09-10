#include "Engine/Renderer/Textures/Texture2D.h"

#include "Engine/Assets/Gtex.h"
#include "Engine/Renderer/Vulkan/buffer.h"
#include "Engine/Renderer/Vulkan/upload_context.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Engine {
    namespace {
        [[nodiscard]] VkFormat to_vk_format(const Assets::TextureFormat format) {
            switch (format) {
                case Assets::TextureFormat::RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
                case Assets::TextureFormat::RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
                case Assets::TextureFormat::BC4_UNORM: return VK_FORMAT_BC4_UNORM_BLOCK;
                case Assets::TextureFormat::BC5_UNORM: return VK_FORMAT_BC5_UNORM_BLOCK;
                case Assets::TextureFormat::BC7_UNORM: return VK_FORMAT_BC7_UNORM_BLOCK;
                case Assets::TextureFormat::BC7_SRGB: return VK_FORMAT_BC7_SRGB_BLOCK;
            }
            throw std::invalid_argument("Unknown cooked texture format");
        }

        void copy_pixels_in_chunks(UploadContext& upload, VkImage image, const std::uint32_t width,
                                   const std::uint32_t height, const std::size_t bytesPerPixel,
                                   const std::span<const std::uint8_t> pixels) {
            const auto pixelsPerChunk = std::max<VkDeviceSize>(1, upload.capacity() / bytesPerPixel);
            for (std::uint32_t y = 0; y < height;) {
                const auto chunkWidth = static_cast<std::uint32_t>(std::min<VkDeviceSize>(width, pixelsPerChunk));
                const auto chunkHeight = static_cast<std::uint32_t>(std::min<VkDeviceSize>(height - y, pixelsPerChunk / chunkWidth));
                for (std::uint32_t x = 0; x < width; x += chunkWidth) {
                    const auto actualWidth = std::min(chunkWidth, width - x);
                    const std::size_t chunkBytes = static_cast<std::size_t>(actualWidth) * chunkHeight * bytesPerPixel;
                    const auto slice = upload.allocate(chunkBytes, 16);
                    auto* destination = static_cast<std::uint8_t*>(slice.mapped);
                    for (std::uint32_t row = 0; row < chunkHeight; ++row) {
                        const auto sourceOffset = (static_cast<std::size_t>(y + row) * width + x) * bytesPerPixel;
                        std::memcpy(destination + static_cast<std::size_t>(row) * actualWidth * bytesPerPixel,
                                    pixels.data() + sourceOffset, static_cast<std::size_t>(actualWidth) * bytesPerPixel);
                    }
                    VkBufferImageCopy copy{};
                    copy.bufferOffset = slice.offset;
                    copy.bufferRowLength = actualWidth;
                    copy.bufferImageHeight = chunkHeight;
                    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    copy.imageOffset = {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), 0};
                    copy.imageExtent = {actualWidth, chunkHeight, 1};
                    vkCmdCopyBufferToImage(upload.commandBuffer(), slice.buffer, image,
                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                }
                y += chunkHeight;
            }
        }

        [[nodiscard]] std::uint32_t bytes_per_block(const Assets::TextureFormat format) {
            if (format == Assets::TextureFormat::RGBA8_SRGB || format == Assets::TextureFormat::RGBA8_UNORM) return 4;
            return format == Assets::TextureFormat::BC4_UNORM ? 8 : 16;
        }

        [[nodiscard]] std::uint32_t block_extent(const Assets::TextureFormat format) {
            return (format == Assets::TextureFormat::RGBA8_SRGB || format == Assets::TextureFormat::RGBA8_UNORM) ? 1 : 4;
        }

        void copy_cooked_in_chunks(UploadContext& upload, VkImage image, const Assets::CookedTexture& texture) {
            const auto bytesPerBlock = bytes_per_block(texture.format);
            const auto blockExtent = block_extent(texture.format);
            const auto blocksPerChunk = std::max<VkDeviceSize>(1, upload.capacity() / bytesPerBlock);
            for (std::uint32_t level = 0; level < texture.mips.size(); ++level) {
                const auto& mip = texture.mips[level];
                const auto blocksWide = (mip.width + blockExtent - 1) / blockExtent;
                const auto blocksHigh = (mip.height + blockExtent - 1) / blockExtent;
                const auto chunkBlocksWide = static_cast<std::uint32_t>(std::min<VkDeviceSize>(blocksWide, blocksPerChunk));
                const auto chunkBlockRows = static_cast<std::uint32_t>(std::max<VkDeviceSize>(1, blocksPerChunk / chunkBlocksWide));
                const auto* source = texture.data.data() + static_cast<std::size_t>(mip.offset);
                for (std::uint32_t blockY = 0; blockY < blocksHigh; blockY += chunkBlockRows) {
                    const auto actualBlockRows = std::min(chunkBlockRows, blocksHigh - blockY);
                    for (std::uint32_t blockX = 0; blockX < blocksWide; blockX += chunkBlocksWide) {
                        const auto actualBlocksWide = std::min(chunkBlocksWide, blocksWide - blockX);
                        const std::size_t chunkBytes = static_cast<std::size_t>(actualBlocksWide) * actualBlockRows * bytesPerBlock;
                        const auto slice = upload.allocate(chunkBytes, 16);
                        auto* destination = static_cast<std::uint8_t*>(slice.mapped);
                        for (std::uint32_t row = 0; row < actualBlockRows; ++row) {
                            const auto sourceOffset = (static_cast<std::size_t>(blockY + row) * blocksWide + blockX) * bytesPerBlock;
                            std::memcpy(destination + static_cast<std::size_t>(row) * actualBlocksWide * bytesPerBlock,
                                        source + sourceOffset, static_cast<std::size_t>(actualBlocksWide) * bytesPerBlock);
                        }
                        VkBufferImageCopy copy{};
                        copy.bufferOffset = slice.offset;
                        copy.bufferRowLength = actualBlocksWide * blockExtent;
                        copy.bufferImageHeight = actualBlockRows * blockExtent;
                        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
                        copy.imageOffset = {static_cast<std::int32_t>(blockX * blockExtent), static_cast<std::int32_t>(blockY * blockExtent), 0};
                        copy.imageExtent = {std::min(actualBlocksWide * blockExtent, mip.width - blockX * blockExtent),
                                            std::min(actualBlockRows * blockExtent, mip.height - blockY * blockExtent), 1};
                        vkCmdCopyBufferToImage(upload.commandBuffer(), slice.buffer, image,
                                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                    }
                }
            }
        }

        void copy_gtex_mips_to_ring(UploadContext& upload, VkImage image, const Assets::GtexTexture& texture,
                                    const std::uint32_t firstMip) {
            const auto bytesPerBlock = bytes_per_block(texture.format);
            const auto blockExtent = block_extent(texture.format);
            for (std::uint32_t sourceLevel = firstMip; sourceLevel < texture.mips.size(); ++sourceLevel) {
                const auto& mip = texture.mips[sourceLevel];
                const auto blocksWide = (mip.width + blockExtent - 1) / blockExtent;
                const auto blocksHigh = (mip.height + blockExtent - 1) / blockExtent;
                const auto rowBytes = static_cast<VkDeviceSize>(blocksWide) * bytesPerBlock;
                const auto rowsPerSlice = std::max<VkDeviceSize>(1, upload.capacity() / rowBytes);
                for (std::uint32_t blockY = 0; blockY < blocksHigh;) {
                    const auto rows = static_cast<std::uint32_t>(std::min<VkDeviceSize>(rowsPerSlice, blocksHigh - blockY));
                    const auto bytes = rowBytes * rows;
                    const auto slice = upload.allocate(bytes, 16);
                    auto destination = std::span<std::uint8_t>{static_cast<std::uint8_t*>(slice.mapped), static_cast<std::size_t>(bytes)};
                    if (!Assets::readMipRange(texture, sourceLevel, static_cast<std::uint64_t>(blockY) * rowBytes, destination))
                        throw std::runtime_error("Could not read GTEX mip payload into upload ring");
                    VkBufferImageCopy copy{};
                    copy.bufferOffset = slice.offset;
                    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, sourceLevel - firstMip, 0, 1};
                    copy.imageOffset = {0, static_cast<std::int32_t>(blockY * blockExtent), 0};
                    copy.imageExtent = {mip.width, std::min(rows * blockExtent, mip.height - blockY * blockExtent), 1};
                    vkCmdCopyBufferToImage(upload.commandBuffer(), slice.buffer, image,
                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                    blockY += rows;
                }
            }
        }

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
        if (this == &other) { return *this; }

        destroy();
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        image_ = std::exchange(other.image_, VK_NULL_HANDLE);
        memory_ = std::exchange(other.memory_, VK_NULL_HANDLE);
        allocation_ = std::exchange(other.allocation_, VK_NULL_HANDLE);
        allocator_ = std::exchange(other.allocator_, VK_NULL_HANDLE);
        imageView_ = std::exchange(other.imageView_, VK_NULL_HANDLE);
        sampler_ = std::exchange(other.sampler_, VK_NULL_HANDLE);
        format_ = std::exchange(other.format_, VK_FORMAT_UNDEFINED);
        width_ = std::exchange(other.width_, 0);
        height_ = std::exchange(other.height_, 0);
        mipLevels_ = std::exchange(other.mipLevels_, 0);
        readyTimeline_ = std::exchange(other.readyTimeline_, 0);
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
        const bool generateMipmaps,
        const VmaAllocator allocator,
        const TexturePixelFormat pixelFormat) {
        if (physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE ||
            commandPool == VK_NULL_HANDLE || queue == VK_NULL_HANDLE) {
            throw std::invalid_argument("Texture2D requires valid Vulkan handles");
        }
        if (width == 0 || height == 0) {
            throw std::invalid_argument("Texture2D dimensions cannot be zero");
        }
        const std::size_t bytesPerPixel = pixelFormat == TexturePixelFormat::R8 ? 1u : 4u;
        if (width > std::numeric_limits<std::size_t>::max() / bytesPerPixel / height) {
            throw std::invalid_argument("Texture2D dimensions are too large");
        }

        const std::size_t expectedSize = static_cast<std::size_t>(width) * height * bytesPerPixel;
        if (rgbaPixels.size() != expectedSize) {
            throw std::invalid_argument("Texture2D pixel data has an invalid size");
        }

        const VkFormat format = pixelFormat == TexturePixelFormat::R8
                                    ? VK_FORMAT_R8_UNORM
                                    : (colorSpace == TextureColorSpace::SRGB
                                           ? VK_FORMAT_R8G8B8A8_SRGB
                                           : VK_FORMAT_R8G8B8A8_UNORM);
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
        allocator_ = allocator;
        width_ = width;
        height_ = height;
        mipLevels_ = mipLevels;

        Buffer staging;
        UploadContext* upload = UploadContext::current();
        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceSize stagingOffset = 0;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
            const bool ownsUploadBatch = upload != nullptr && !upload->recording();
            try {
                if (upload != nullptr) {
                    if (ownsUploadBatch) upload->begin();
                    commandBuffer = upload->commandBuffer();
            } else {
                staging.createHostVisible(physicalDevice, device_, static_cast<VkDeviceSize>(expectedSize), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, allocator);
                staging.update(rgbaPixels.data(), static_cast<VkDeviceSize>(expectedSize));
                stagingBuffer = staging.handle();
            }

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
            if (allocator_ == VK_NULL_HANDLE) {
                throw std::invalid_argument("Texture2D requires a VMA allocator");
            }
            VmaAllocationCreateInfo allocationInfo{};
            allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            if (vmaCreateImage(allocator_, &imageInfo, &allocationInfo, &image_,
                               &allocation_, nullptr) != VK_SUCCESS) {
                throw std::runtime_error("Could not allocate Texture2D image with VMA");
            }
            VmaAllocationInfo allocationDetails{};
            vmaGetAllocationInfo(allocator_, allocation_, &allocationDetails);
            memory_ = allocationDetails.deviceMemory;

            if (upload == nullptr) {
                VkCommandBufferAllocateInfo commandAllocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; commandAllocation.commandPool = commandPool; commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; commandAllocation.commandBufferCount = 1;
                if (vkAllocateCommandBuffers(device_, &commandAllocation, &commandBuffer) != VK_SUCCESS) throw std::runtime_error("Could not allocate Texture2D upload command buffer");
                VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) throw std::runtime_error("Could not begin Texture2D upload command buffer");
            }

            transitionImage(
                commandBuffer, image_, 0, mipLevels_,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

            if (upload != nullptr) {
                copy_pixels_in_chunks(*upload, image_, width_, height_, bytesPerPixel, rgbaPixels);
                commandBuffer = upload->commandBuffer();
            } else {
                VkBufferImageCopy copy{};
                copy.bufferOffset = stagingOffset;
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent = {width_, height_, 1};
                vkCmdCopyBufferToImage(
                    commandBuffer, stagingBuffer, image_,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            }

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

            if (upload != nullptr) {
                readyTimeline_ = upload->pendingTicket().timelineValue;
                if (ownsUploadBatch) readyTimeline_ = upload->submit().timelineValue;
                commandBuffer = VK_NULL_HANDLE;
            }
            else { if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) throw std::runtime_error("Could not end Texture2D upload command buffer"); VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &commandBuffer; if (vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) throw std::runtime_error("Could not upload Texture2D"); vkQueueWaitIdle(queue); vkFreeCommandBuffers(device_, commandPool, 1, &commandBuffer); commandBuffer = VK_NULL_HANDLE; }

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
            samplerInfo.minLod = 0.0F;
            samplerInfo.maxLod = static_cast<float>(mipLevels_ - 1);
            if (vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_) != VK_SUCCESS) {
                throw std::runtime_error("Could not create Texture2D sampler");
            }
        } catch (...) {
            if (commandBuffer != VK_NULL_HANDLE && upload == nullptr) {
                vkFreeCommandBuffers(device_, commandPool, 1, &commandBuffer);
            }
            destroy();
            throw;
        }
    }

    void Texture2D::createCooked(
        const VkPhysicalDevice physicalDevice, const VkDevice device, const VkCommandPool commandPool,
        const VkQueue queue, const Assets::CookedTexture& texture, const VmaAllocator allocator) {
        if (physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE || commandPool == VK_NULL_HANDLE || queue == VK_NULL_HANDLE ||
            allocator == VK_NULL_HANDLE) throw std::invalid_argument("Texture2D requires valid Vulkan handles and VMA allocator");
        if (!Assets::valid_cooked_texture(texture)) throw std::invalid_argument("Cooked texture has invalid mip data");

        destroy();
        device_ = device;
        allocator_ = allocator;
        format_ = to_vk_format(texture.format);
        width_ = texture.width;
        height_ = texture.height;
        mipLevels_ = static_cast<std::uint32_t>(texture.mips.size());

        Buffer staging;
        UploadContext* upload = UploadContext::current();
        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceSize stagingOffset = 0;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
            const bool ownsUploadBatch = upload != nullptr && !upload->recording();
            try {
                if (upload != nullptr) {
                    if (ownsUploadBatch) upload->begin();
                    commandBuffer = upload->commandBuffer();
            } else {
                staging.createHostVisible(physicalDevice, device_, static_cast<VkDeviceSize>(texture.data.size()), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, allocator_);
                staging.update(texture.data.data(), static_cast<VkDeviceSize>(texture.data.size()));
                stagingBuffer = staging.handle();
            }

            VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = format_;
            imageInfo.extent = {width_, height_, 1};
            imageInfo.mipLevels = mipLevels_;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VmaAllocationCreateInfo allocationInfo{};
            allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            if (vmaCreateImage(allocator_, &imageInfo, &allocationInfo, &image_, &allocation_, nullptr) != VK_SUCCESS)
                throw std::runtime_error("Could not allocate cooked Texture2D image with VMA");
            VmaAllocationInfo allocationDetails{};
            vmaGetAllocationInfo(allocator_, allocation_, &allocationDetails);
            memory_ = allocationDetails.deviceMemory;

            if (upload == nullptr) {
                VkCommandBufferAllocateInfo commandAllocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                commandAllocation.commandPool = commandPool; commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; commandAllocation.commandBufferCount = 1;
                VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                if (vkAllocateCommandBuffers(device_, &commandAllocation, &commandBuffer) != VK_SUCCESS ||
                    vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS)
                    throw std::runtime_error("Could not begin cooked Texture2D upload command buffer");
            }

            transitionImage(commandBuffer, image_, 0, mipLevels_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            if (upload != nullptr) {
                copy_cooked_in_chunks(*upload, image_, texture);
                commandBuffer = upload->commandBuffer();
            } else {
                std::vector<VkBufferImageCopy> regions;
                regions.reserve(texture.mips.size());
                for (std::uint32_t level = 0; level < mipLevels_; ++level) {
                    const auto& mip = texture.mips[level];
                    VkBufferImageCopy copy{};
                    copy.bufferOffset = stagingOffset + mip.offset;
                    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
                    copy.imageExtent = {mip.width, mip.height, 1};
                    regions.push_back(copy);
                }
                vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       static_cast<std::uint32_t>(regions.size()), regions.data());
            }
            transitionImage(commandBuffer, image_, 0, mipLevels_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            if (upload != nullptr) {
                readyTimeline_ = upload->pendingTicket().timelineValue;
                if (ownsUploadBatch) readyTimeline_ = upload->submit().timelineValue;
                commandBuffer = VK_NULL_HANDLE;
            } else {
                if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) throw std::runtime_error("Could not end cooked Texture2D upload command buffer");
                VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &commandBuffer;
                if (vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) throw std::runtime_error("Could not upload cooked Texture2D");
                vkQueueWaitIdle(queue); vkFreeCommandBuffers(device_, commandPool, 1, &commandBuffer); commandBuffer = VK_NULL_HANDLE;
            }
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = image_; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format_;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels_, 0, 1};
            if (vkCreateImageView(device_, &view, nullptr, &imageView_) != VK_SUCCESS) throw std::runtime_error("Could not create cooked Texture2D image view");
            VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            samplerInfo.magFilter = VK_FILTER_LINEAR; samplerInfo.minFilter = VK_FILTER_LINEAR; samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT; samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT; samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerInfo.maxLod = static_cast<float>(mipLevels_ - 1);
            if (vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_) != VK_SUCCESS) throw std::runtime_error("Could not create cooked Texture2D sampler");
        } catch (...) {
            if (commandBuffer != VK_NULL_HANDLE && upload == nullptr) vkFreeCommandBuffers(device_, commandPool, 1, &commandBuffer);
            destroy();
            throw;
        }
    }

    void Texture2D::createGtex(
        const VkPhysicalDevice physicalDevice, const VkDevice device, const VkCommandPool commandPool,
        const VkQueue queue, const Assets::GtexTexture& texture, const std::uint32_t firstResidentMip,
        const VmaAllocator allocator) {
        if (physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE || commandPool == VK_NULL_HANDLE || queue == VK_NULL_HANDLE ||
            allocator == VK_NULL_HANDLE || firstResidentMip >= texture.mips.size())
            throw std::invalid_argument("Texture2D::createGtex received invalid arguments");
        UploadContext* const upload = UploadContext::current();
        if (upload == nullptr) throw std::logic_error("GTEX streaming requires an active UploadContext");

        destroy();
        device_ = device;
        allocator_ = allocator;
        format_ = to_vk_format(texture.format);
        width_ = texture.mips[firstResidentMip].width;
        height_ = texture.mips[firstResidentMip].height;
        mipLevels_ = static_cast<std::uint32_t>(texture.mips.size()) - firstResidentMip;
        const bool ownsUploadBatch = !upload->recording();
        try {
            if (ownsUploadBatch) upload->begin();
            VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = format_;
            imageInfo.extent = {width_, height_, 1};
            imageInfo.mipLevels = mipLevels_;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VmaAllocationCreateInfo allocationInfo{};
            allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            if (vmaCreateImage(allocator_, &imageInfo, &allocationInfo, &image_, &allocation_, nullptr) != VK_SUCCESS)
                throw std::runtime_error("Could not allocate resident GTEX Texture2D image");
            VmaAllocationInfo allocationDetails{};
            vmaGetAllocationInfo(allocator_, allocation_, &allocationDetails);
            memory_ = allocationDetails.deviceMemory;

            transitionImage(upload->commandBuffer(), image_, 0, mipLevels_, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            copy_gtex_mips_to_ring(*upload, image_, texture, firstResidentMip);
            transitionImage(upload->commandBuffer(), image_, 0, mipLevels_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            readyTimeline_ = upload->pendingTicket().timelineValue;
            if (ownsUploadBatch) readyTimeline_ = upload->submit().timelineValue;

            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = image_; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format_;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels_, 0, 1};
            if (vkCreateImageView(device_, &view, nullptr, &imageView_) != VK_SUCCESS)
                throw std::runtime_error("Could not create resident GTEX image view");
            VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            samplerInfo.magFilter = VK_FILTER_LINEAR; samplerInfo.minFilter = VK_FILTER_LINEAR;
            samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT; samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT; samplerInfo.maxLod = static_cast<float>(mipLevels_ - 1);
            if (vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_) != VK_SUCCESS)
                throw std::runtime_error("Could not create resident GTEX sampler");
        } catch (...) {
            destroy();
            throw;
        }
    }

    void Texture2D::createFromAsset(
        const VkPhysicalDevice physicalDevice, const VkDevice device, const VkCommandPool commandPool,
        const VkQueue queue, const Assets::TextureAsset& asset, const TextureColorSpace colorSpace,
        const VmaAllocator allocator) {
        if (asset.cooked) {
            createCooked(physicalDevice, device, commandPool, queue, *asset.cooked, allocator);
            return;
        }
        if (asset.gtex) {
            // Start from the smallest level: a residency manager can promote this later.
            createGtex(physicalDevice, device, commandPool, queue, *asset.gtex,
                       static_cast<std::uint32_t>(asset.gtex->mips.size() - 1), allocator);
            return;
        }
        create(physicalDevice, device, commandPool, queue, asset.width, asset.height, asset.rgbaPixels,
               colorSpace, true, allocator);
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
                if (allocator_ != VK_NULL_HANDLE && allocation_ != VK_NULL_HANDLE) {
                    vmaDestroyImage(allocator_, image_, allocation_);
                } else {
                    vkDestroyImage(device_, image_, nullptr);
                }
            }
        }
        device_ = VK_NULL_HANDLE;
        image_ = VK_NULL_HANDLE;
        memory_ = VK_NULL_HANDLE;
        allocation_ = VK_NULL_HANDLE;
        allocator_ = VK_NULL_HANDLE;
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
