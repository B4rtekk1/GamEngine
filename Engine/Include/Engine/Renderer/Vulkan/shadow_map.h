#pragma once

#include <vulkan/vulkan.h>

namespace Engine {
    class ShadowMap final {
    public:
        static constexpr uint32_t TileResolution = 1024;
        static constexpr uint32_t TilesPerAxis = 2;
        static constexpr uint32_t ClipLevelCount = 4;
        static constexpr uint32_t Resolution = TileResolution * TilesPerAxis;

        ~ShadowMap();

        ShadowMap() = default;

        ShadowMap(const ShadowMap &) = delete;

        ShadowMap &operator=(const ShadowMap &) = delete;

        void create(VkPhysicalDevice physicalDevice, VkDevice device);

        void destroy() noexcept;

        [[nodiscard]] VkFormat format() const noexcept { return format_; }
        [[nodiscard]] VkImage image() const noexcept { return image_; }
        [[nodiscard]] VkImageView imageView() const noexcept { return imageView_; }
        [[nodiscard]] VkSampler sampler() const noexcept { return sampler_; }
        [[nodiscard]] VkRenderPass renderPass() const noexcept { return renderPass_; }
        [[nodiscard]] VkFramebuffer framebuffer() const noexcept { return framebuffer_; }

    private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkImage image_ = VK_NULL_HANDLE;
        VkDeviceMemory memory_ = VK_NULL_HANDLE;
        VkImageView imageView_ = VK_NULL_HANDLE;
        VkSampler sampler_ = VK_NULL_HANDLE;
        VkRenderPass renderPass_ = VK_NULL_HANDLE;
        VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
        VkFormat format_ = VK_FORMAT_UNDEFINED;
    };
} // namespace Engine
