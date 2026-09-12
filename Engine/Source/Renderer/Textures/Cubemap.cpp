#include "Engine/Renderer/Textures/Cubemap.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace Engine {
namespace {
uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
        if ((typeFilter & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) return i;
    throw std::runtime_error("Could not find cubemap memory type");
}

uint16_t floatToHalf(float value) noexcept {
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    const uint32_t sign = (bits >> 16U) & 0x8000U;
    const int exponent = static_cast<int>((bits >> 23U) & 0xffU) - 127 + 15;
    uint32_t mantissa = bits & 0x007fffffU;
    if (exponent <= 0) {
        if (exponent < -10) return static_cast<uint16_t>(sign);
        mantissa = (mantissa | 0x00800000U) >> static_cast<uint32_t>(1 - exponent);
        return static_cast<uint16_t>(sign | ((mantissa + 0x1000U) >> 13U));
    }
    if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7c00U);
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10U) |
                                 ((mantissa + 0x1000U) >> 13U));
}

float srgbToLinear(float value) noexcept {
    return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

void submitAndWait(VkDevice device, VkQueue queue, VkCommandBuffer commandBuffer) {
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) throw std::runtime_error("Could not create cubemap upload fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &commandBuffer;
    const VkResult submitted = vkQueueSubmit(queue, 1, &submit, fence);
    const VkResult completed = submitted == VK_SUCCESS ? vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) : submitted;
    vkDestroyFence(device, fence, nullptr);
    if (completed != VK_SUCCESS) throw std::runtime_error("Could not upload cubemap");
}
}

Cubemap::~Cubemap() { destroy(); }

void Cubemap::create(VkPhysicalDevice physicalDevice, VkDevice device, VkCommandPool commandPool,
                     VkQueue queue, const std::array<std::array<uint8_t, 4>, 6>& faceColours) {
    std::vector<float> pixels(6 * 4);
    for (std::size_t face = 0; face < faceColours.size(); ++face)
        for (std::size_t channel = 0; channel < 4; ++channel)
            pixels[face * 4 + channel] = channel == 3
                ? static_cast<float>(faceColours[face][channel]) / 255.0F
                : srgbToLinear(static_cast<float>(faceColours[face][channel]) / 255.0F);
    createHdr(physicalDevice, device, commandPool, queue, 1, 1, pixels);
}

void Cubemap::createHdr(VkPhysicalDevice physicalDevice, VkDevice device, VkCommandPool commandPool,
                        VkQueue queue, uint32_t faceSize, uint32_t mipLevels, std::span<const float> rgbaPixels) {
    if (faceSize == 0 || mipLevels == 0 || mipLevels > static_cast<uint32_t>(std::bit_width(faceSize))) throw std::invalid_argument("Cubemap has invalid dimensions or mip count");
    std::size_t expectedFloats = 0;
    for (uint32_t mip = 0; mip < mipLevels; ++mip) {
        const auto side = std::max(1U, faceSize >> mip);
        expectedFloats += static_cast<std::size_t>(6) * side * side * 4;
    }
    if (rgbaPixels.size() != expectedFloats) throw std::invalid_argument("Cubemap HDR data has an invalid size");

    destroy(); device_ = device; mipLevels_ = mipLevels;
    VkBuffer staging = VK_NULL_HANDLE; VkDeviceMemory stagingMemory = VK_NULL_HANDLE; VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    try {
        std::vector<uint16_t> halfPixels(rgbaPixels.size());
        std::transform(rgbaPixels.begin(), rgbaPixels.end(), halfPixels.begin(), floatToHalf);
        const VkDeviceSize imageSize = static_cast<VkDeviceSize>(halfPixels.size() * sizeof(uint16_t));
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = imageSize; bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device_, &bufferInfo, nullptr, &staging) != VK_SUCCESS) throw std::runtime_error("Could not create cubemap staging buffer");
        VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(device_, staging, &requirements);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex = findMemoryType(physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(device_, &alloc, nullptr, &stagingMemory) != VK_SUCCESS) throw std::runtime_error("Could not allocate cubemap staging memory");
        if (vkBindBufferMemory(device_, staging, stagingMemory, 0) != VK_SUCCESS) throw std::runtime_error("Could not bind cubemap staging memory");
        void* mapped = nullptr;
        if (vkMapMemory(device_, stagingMemory, 0, imageSize, 0, &mapped) != VK_SUCCESS) throw std::runtime_error("Could not map cubemap staging memory");
        std::memcpy(mapped, halfPixels.data(), static_cast<size_t>(imageSize)); vkUnmapMemory(device_, stagingMemory);

        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        imageInfo.imageType = VK_IMAGE_TYPE_2D; imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        imageInfo.extent = {faceSize, faceSize, 1}; imageInfo.mipLevels = mipLevels_; imageInfo.arrayLayers = 6;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT; imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT; imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(device_, &imageInfo, nullptr, &image_) != VK_SUCCESS) throw std::runtime_error("Could not create cubemap image");
        vkGetImageMemoryRequirements(device_, image_, &requirements); alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex = findMemoryType(physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device_, &alloc, nullptr, &memory_) != VK_SUCCESS) throw std::runtime_error("Could not allocate cubemap image memory");
        if (vkBindImageMemory(device_, image_, memory_, 0) != VK_SUCCESS) throw std::runtime_error("Could not bind cubemap image memory");

        VkCommandBufferAllocateInfo commandAlloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; commandAlloc.commandPool = commandPool; commandAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; commandAlloc.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device_, &commandAlloc, &commandBuffer) != VK_SUCCESS) throw std::runtime_error("Could not allocate cubemap upload command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(commandBuffer, &begin) != VK_SUCCESS) throw std::runtime_error("Could not begin cubemap upload command buffer");
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.image = image_; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels_, 0, 6};
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        std::vector<VkBufferImageCopy> regions; regions.reserve(static_cast<std::size_t>(mipLevels_) * 6); VkDeviceSize offset = 0;
        for (uint32_t mip = 0; mip < mipLevels_; ++mip) {
            const auto side = std::max(1U, faceSize >> mip); const VkDeviceSize faceBytes = static_cast<VkDeviceSize>(side) * side * 4 * sizeof(uint16_t);
            for (uint32_t face = 0; face < 6; ++face) {
                VkBufferImageCopy copy{}; copy.bufferOffset = offset; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, face, 1}; copy.imageExtent = {side, side, 1};
                regions.push_back(copy); offset += faceBytes;
            }
        }
        vkCmdCopyBufferToImage(commandBuffer, staging, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()), regions.data());
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) throw std::runtime_error("Could not finish cubemap upload command buffer");
        submitAndWait(device_, queue, commandBuffer); vkFreeCommandBuffers(device_, commandPool, 1, &commandBuffer); commandBuffer = VK_NULL_HANDLE;

        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = image_; view.viewType = VK_IMAGE_VIEW_TYPE_CUBE; view.format = imageInfo.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels_, 0, 6};
        if (vkCreateImageView(device_, &view, nullptr, &imageView_) != VK_SUCCESS) throw std::runtime_error("Could not create cubemap view");
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sampler.magFilter = VK_FILTER_LINEAR; sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR; sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = static_cast<float>(mipLevels_ - 1);
        if (vkCreateSampler(device_, &sampler, nullptr, &sampler_) != VK_SUCCESS) throw std::runtime_error("Could not create cubemap sampler");
        vkDestroyBuffer(device_, staging, nullptr); vkFreeMemory(device_, stagingMemory, nullptr);
    } catch (...) {
        if (commandBuffer != VK_NULL_HANDLE) vkFreeCommandBuffers(device_, commandPool, 1, &commandBuffer);
        if (staging != VK_NULL_HANDLE) vkDestroyBuffer(device_, staging, nullptr);
        if (stagingMemory != VK_NULL_HANDLE) vkFreeMemory(device_, stagingMemory, nullptr);
        destroy(); throw;
    }
}

void Cubemap::destroy() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        if (sampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, sampler_, nullptr);
        if (imageView_ != VK_NULL_HANDLE) vkDestroyImageView(device_, imageView_, nullptr);
        if (image_ != VK_NULL_HANDLE) vkDestroyImage(device_, image_, nullptr);
        if (memory_ != VK_NULL_HANDLE) vkFreeMemory(device_, memory_, nullptr);
    }
    sampler_ = VK_NULL_HANDLE; imageView_ = VK_NULL_HANDLE; image_ = VK_NULL_HANDLE; memory_ = VK_NULL_HANDLE; device_ = VK_NULL_HANDLE; mipLevels_ = 0;
}
} // namespace Engine
