#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

class DepthBuffer final {
public:
    DepthBuffer() = default;
    ~DepthBuffer();

    DepthBuffer(const DepthBuffer&) = delete;
    DepthBuffer& operator=(const DepthBuffer&) = delete;
    DepthBuffer(DepthBuffer&&) = delete;
    DepthBuffer& operator=(DepthBuffer&&) = delete;

    void initialize(VkPhysicalDevice physicalDevice, VkDevice device);
    void create(VkExtent2D extent, VkSampleCountFlagBits samples);
    void destroy() noexcept;

    [[nodiscard]] VkImage image() const noexcept {
        return image_;
    }

    [[nodiscard]] VkImageView imageView() const noexcept {
        return imageView_;
    }

    [[nodiscard]] VkFormat format() const noexcept {
        return format_;
    }

private:
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;

    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView imageView_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;

    [[nodiscard]] uint32_t findMemoryType(
        uint32_t typeFilter,
        VkMemoryPropertyFlags requiredProperties) const;

    [[nodiscard]] VkFormat findSupportedFormat(
        VkSampleCountFlagBits samples) const;
    [[nodiscard]] static bool hasStencilComponent(VkFormat format) noexcept;
};