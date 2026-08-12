#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>

namespace Engine {

// GPU cubemap populated from one RGBA colour per face.  Keeping the upload
// here makes Skybox independent of an image-loading library.
class Cubemap final {
public:
    Cubemap() = default;
    ~Cubemap();

    Cubemap(const Cubemap&) = delete;
    Cubemap& operator=(const Cubemap&) = delete;

    void create(VkPhysicalDevice physicalDevice, VkDevice device, VkCommandPool commandPool,
                VkQueue queue, const std::array<std::array<uint8_t, 4>, 6>& faceColours);
    void destroy() noexcept;

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
