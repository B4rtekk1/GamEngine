#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <span>
#include <utility>

namespace Engine {

// GPU cubemap populated from one RGBA colour per face.  Keeping the upload
// here makes Skybox independent of an image-loading library.
class Cubemap final {
public:
    Cubemap() = default;
    ~Cubemap();

    Cubemap(const Cubemap&) = delete;
    Cubemap& operator=(const Cubemap&) = delete;
    Cubemap(Cubemap&& other) noexcept;
    Cubemap& operator=(Cubemap&& other) noexcept;

    void create(VkPhysicalDevice physicalDevice, VkDevice device, VkCommandPool commandPool,
                VkQueue queue, const std::array<std::array<uint8_t, 4>, 6>& faceColours); //NOLINT

    // Uploads linear HDR texels ordered by mip, face, then row-major texel.
    void createHdr(VkPhysicalDevice physicalDevice, VkDevice device, VkCommandPool commandPool,
                   VkQueue queue, std::uint32_t faceSize, std::uint32_t mipLevels,
                   std::span<const float> rgbaPixels);

    /** Allocates an empty HDR cubemap that can be rendered into one face at a
     * time.  The image starts undefined; the capture render pass owns its
     * first layout transition. */
    void createRenderTarget(VkPhysicalDevice physicalDevice, VkDevice device,
                            std::uint32_t faceSize, std::uint32_t mipLevels);
    void destroy() noexcept;

    [[nodiscard]] VkImageView imageView() const noexcept { return imageView_; }
    [[nodiscard]] VkImageView faceImageView(std::uint32_t face) const noexcept {
        return face < faceImageViews_.size() ? faceImageViews_[face] : VK_NULL_HANDLE;
    }
    [[nodiscard]] VkImage image() const noexcept { return image_; }
    [[nodiscard]] VkSampler sampler() const noexcept { return sampler_; }
    [[nodiscard]] std::uint32_t mipLevels() const noexcept { return mipLevels_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView imageView_ = VK_NULL_HANDLE;
    std::array<VkImageView, 6> faceImageViews_{};
    VkSampler sampler_ = VK_NULL_HANDLE;
    std::uint32_t mipLevels_ = 0;
};

} // namespace Engine
