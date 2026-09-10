#pragma once

#include "Engine/Renderer/Vulkan/hdr_buffer.h"
#include <vk_mem_alloc.h>

namespace Engine {
    // A resizable persistent off-screen color target. Per-view depth and MSAA
    // attachments live in ViewRenderScratchResources and are reused by the
    // currently rendered viewport.
    class ViewportRenderTarget final {
    public:
        ViewportRenderTarget() = default;

        ~ViewportRenderTarget() = default;

        ViewportRenderTarget(const ViewportRenderTarget &) = delete;

        ViewportRenderTarget &operator=(const ViewportRenderTarget &) = delete;

        void create(VkPhysicalDevice physicalDevice, VkDevice device, VkExtent2D extent,
                    VmaAllocator allocator);

        void resize(VkExtent2D extent);

        void destroy() noexcept;

        [[nodiscard]] VkExtent2D extent() const noexcept { return extent_; }
        [[nodiscard]] const HdrBuffer &color() const noexcept { return color_; }

        [[nodiscard]] VkDescriptorImageInfo colorDescriptor() const noexcept;

    private:
        VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
        VkDevice device_ = VK_NULL_HANDLE;
        VmaAllocator allocator_ = VK_NULL_HANDLE;
        VkExtent2D extent_{};
        HdrBuffer color_;
    };
} // namespace Engine
