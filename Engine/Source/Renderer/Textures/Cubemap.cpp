#include "Engine/Renderer/Textures/Cubemap.h"

#include <cstring>
#include <stdexcept>

namespace Engine {
namespace {
uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter,
                        VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) &&
            (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) return i;
    }
    throw std::runtime_error("Could not find cubemap memory type");
}
}

Cubemap::~Cubemap() { destroy(); }

void Cubemap::create(VkPhysicalDevice physicalDevice, VkDevice device, VkCommandPool commandPool,
                     VkQueue queue, const std::array<std::array<uint8_t, 4>, 6>& faceColours) {
    destroy();
    device_ = device;
    constexpr VkDeviceSize imageSize = 4 * 6;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    try {
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = imageSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device_, &bufferInfo, nullptr, &staging) != VK_SUCCESS) throw std::runtime_error("Could not create cubemap staging buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, staging, &requirements);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex = findMemoryType(physicalDevice, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(device_, &alloc, nullptr, &stagingMemory) != VK_SUCCESS) throw std::runtime_error("Could not allocate cubemap staging memory");
        vkBindBufferMemory(device_, staging, stagingMemory, 0);
        void* mapped = nullptr;
        if (vkMapMemory(device_, stagingMemory, 0, imageSize, 0, &mapped) != VK_SUCCESS) throw std::runtime_error("Could not map cubemap staging memory");
        std::memcpy(mapped, faceColours.data(), static_cast<size_t>(imageSize));
        vkUnmapMemory(device_, stagingMemory);

        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {1, 1, 1}; imageInfo.mipLevels = 1; imageInfo.arrayLayers = 6;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT; imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE; imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device_, &imageInfo, nullptr, &image_) != VK_SUCCESS) throw std::runtime_error("Could not create cubemap image");
        vkGetImageMemoryRequirements(device_, image_, &requirements);
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex = findMemoryType(physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device_, &alloc, nullptr, &memory_) != VK_SUCCESS) throw std::runtime_error("Could not allocate cubemap image memory");
        if (vkBindImageMemory(device_, image_, memory_, 0) != VK_SUCCESS) throw std::runtime_error("Could not bind cubemap image memory");

        VkCommandBufferAllocateInfo commandAlloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandAlloc.commandPool = commandPool; commandAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; commandAlloc.commandBufferCount = 1;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(device_, &commandAlloc, &commandBuffer) != VK_SUCCESS) throw std::runtime_error("Could not allocate cubemap upload command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commandBuffer, &begin);
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcAccessMask = 0; barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.image = image_; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 6}; copy.imageExtent = {1, 1, 1};
        vkCmdCopyBufferToImage(commandBuffer, staging, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) throw std::runtime_error("Could not finish cubemap upload command buffer");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &commandBuffer;
        if (vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS || vkQueueWaitIdle(queue) != VK_SUCCESS) throw std::runtime_error("Could not upload cubemap");
        vkFreeCommandBuffers(device_, commandPool, 1, &commandBuffer);

        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image_; view.viewType = VK_IMAGE_VIEW_TYPE_CUBE; view.format = imageInfo.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
        if (vkCreateImageView(device_, &view, nullptr, &imageView_) != VK_SUCCESS) throw std::runtime_error("Could not create cubemap view");
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = VK_FILTER_LINEAR; sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR; sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = 1.0f;
        if (vkCreateSampler(device_, &sampler, nullptr, &sampler_) != VK_SUCCESS) throw std::runtime_error("Could not create cubemap sampler");
        vkFreeMemory(device_, stagingMemory, nullptr); vkDestroyBuffer(device_, staging, nullptr);
    } catch (...) {
        if (stagingMemory != VK_NULL_HANDLE) vkFreeMemory(device_, stagingMemory, nullptr);
        if (staging != VK_NULL_HANDLE) vkDestroyBuffer(device_, staging, nullptr);
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
    sampler_ = VK_NULL_HANDLE; imageView_ = VK_NULL_HANDLE; image_ = VK_NULL_HANDLE; memory_ = VK_NULL_HANDLE; device_ = VK_NULL_HANDLE;
}
} // namespace Engine
