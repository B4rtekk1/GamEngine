#pragma once

#include "Engine/Assets/AssetTypes.h"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <span>

namespace Engine {

enum class TextureColorSpace:uint8_t {
    Linear,
    SRGB,
};

enum class TexturePixelFormat:uint8_t {
    RGBA8,
    R8,
    /** Two 16-bit floating-point channels, for data textures such as a BRDF LUT. */
    RG16F,
};

// Owns a sampled image uploaded to device-local memory and ready for sampling.
// Image decoding intentionally stays outside this class so callers may use any
// asset library (for example stb_image or a glTF importer).
class Texture2D final {
public:
    Texture2D() = default;
    ~Texture2D();

    Texture2D(const Texture2D&) = delete;
    Texture2D& operator=(const Texture2D&) = delete;

    Texture2D(Texture2D&& other) noexcept;
    Texture2D& operator=(Texture2D&& other) noexcept;

    void create(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkCommandPool commandPool,
        VkQueue queue,
        std::uint32_t width,
        std::uint32_t height,
        std::span<const std::uint8_t> rgbaPixels,
        TextureColorSpace colorSpace = TextureColorSpace::SRGB,
        bool generateMipmaps = true,
        VmaAllocator allocator = VK_NULL_HANDLE,
        TexturePixelFormat pixelFormat = TexturePixelFormat::RGBA8);

    /** Uploads pre-cooked RGBA/BC mip blocks without runtime decompression or blits. */
    void createCooked(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkCommandPool commandPool,
        VkQueue queue,
        const Assets::CookedTexture& texture,
        VmaAllocator allocator = VK_NULL_HANDLE);

    /**
     * Creates one persistent image with the complete GTEX mip chain and uploads
     * the tail beginning at firstResidentMip.  Later promotions do not replace
     * the image, its allocation, or its image view.
     */
    void createGtex(
        VkPhysicalDevice physicalDevice, VkDevice device, VkCommandPool commandPool, VkQueue queue,
        const Assets::GtexTexture& texture, std::uint32_t firstResidentMip,
        VmaAllocator allocator = VK_NULL_HANDLE);

    /**
     * Uploads missing, sharper GTEX mip levels into an existing persistent
     * image. newFirstResidentMip must be no greater than residentFirstMip().
     * Sampling must be LOD-clamped by the residency consumer until this upload
     * has completed; the texture descriptor itself remains stable.
     */
    void promoteGtex(
        VkCommandPool commandPool, VkQueue queue, const Assets::GtexTexture& texture,
        std::uint32_t newFirstResidentMip);

    /** Uses the cooked payload when available; decoded RGBA remains a development fallback. */
    void createFromAsset(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkCommandPool commandPool,
        VkQueue queue,
        const Assets::TextureAsset& asset,
        TextureColorSpace colorSpace = TextureColorSpace::SRGB,
        VmaAllocator allocator = VK_NULL_HANDLE);

    void destroy() noexcept;

    [[nodiscard]] VkImage image() const noexcept { return image_; }
    [[nodiscard]] VkImageView imageView() const noexcept { return imageView_; }
    [[nodiscard]] VkSampler sampler() const noexcept { return sampler_; }
    [[nodiscard]] VkFormat format() const noexcept { return format_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] std::uint32_t mipLevels() const noexcept { return mipLevels_; }
    [[nodiscard]] std::uint32_t residentFirstMip() const noexcept { return residentFirstMip_; }
    [[nodiscard]] bool valid() const noexcept { return image_ != VK_NULL_HANDLE; }
    [[nodiscard]] std::uint64_t readyTimeline() const noexcept { return readyTimeline_; }

private:
    [[nodiscard]] static std::uint32_t findMemoryType(
        VkPhysicalDevice physicalDevice,
        std::uint32_t typeFilter,
        VkMemoryPropertyFlags properties);

    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkImageView imageView_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t mipLevels_ = 0;
    std::uint32_t residentFirstMip_ = 0;
    std::uint64_t readyTimeline_ = 0;
};

} // namespace Engine
