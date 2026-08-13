#pragma once

#include <vulkan/vulkan.h>

namespace Engine {

class HdrBuffer final {
public:
    static constexpr VkFormat Format = VK_FORMAT_R16G16B16A16_SFLOAT;

    HdrBuffer() = default;
    ~HdrBuffer();

    HdrBuffer(const HdrBuffer&) = delete;
    HdrBuffer& operator=(const HdrBuffer&) = delete;

    void create(VkPhysicalDevice physicalDevice, VkDevice device, VkExtent2D extent);
    void destroy() noexcept;

    [[nodiscard]] VkImage image() const noexcept { return image_; }
    [[nodiscard]] VkImageView imageView() const noexcept { return imageView_; }
    [[nodiscard]] VkSampler sampler() const noexcept { return sampler_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView imageView_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
};

} // namespace Engine
