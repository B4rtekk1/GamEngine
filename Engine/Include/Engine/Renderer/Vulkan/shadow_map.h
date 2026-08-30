#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace Engine {
    class ShadowMap final {
    public:
        static constexpr uint32_t ClipLevelCount = 4;
        static constexpr uint32_t PageResolution = 256;
        static constexpr uint32_t VirtualPagesPerAxis = 32;
        static constexpr uint32_t PhysicalPagesPerAxis = 16;
        /// Maximum number of virtual pages refreshed in a single frame.
        static constexpr uint32_t MaxPageUpdatesPerFrame = 48;
        static constexpr uint32_t VirtualResolution =
            PageResolution * VirtualPagesPerAxis;
        static constexpr uint32_t Resolution =
            PageResolution * PhysicalPagesPerAxis;
        static constexpr uint32_t VirtualPageCount = ClipLevelCount *
            VirtualPagesPerAxis * VirtualPagesPerAxis;
        static constexpr uint32_t PhysicalPageCount =
            PhysicalPagesPerAxis * PhysicalPagesPerAxis;
        static constexpr uint32_t InvalidPage = 0xffffffffu;

        ~ShadowMap();

        ShadowMap() = default;

        ShadowMap(const ShadowMap &) = delete;

        ShadowMap &operator=(const ShadowMap &) = delete;

        void create(VkPhysicalDevice physicalDevice, VkDevice device, VmaAllocator allocator);

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
        VmaAllocation allocation_ = VK_NULL_HANDLE;
        VmaAllocator allocator_ = VK_NULL_HANDLE;
        VkImageView imageView_ = VK_NULL_HANDLE;
        VkSampler sampler_ = VK_NULL_HANDLE;
        VkRenderPass renderPass_ = VK_NULL_HANDLE;
        VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
        VkFormat format_ = VK_FORMAT_UNDEFINED;
    };
} // namespace Engine
