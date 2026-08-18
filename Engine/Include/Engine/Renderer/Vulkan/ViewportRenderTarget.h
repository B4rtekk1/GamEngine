#pragma once

#include "Engine/Renderer/Vulkan/depth_buffer.h"
#include "Engine/Renderer/Vulkan/hdr_buffer.h"

namespace Engine {

// A resizable off-screen target. The viewport UI samples color() while the
// renderer uses depth() as its depth attachment.
class ViewportRenderTarget final {
public:
    ViewportRenderTarget() = default;
    ~ViewportRenderTarget() = default;

    ViewportRenderTarget(const ViewportRenderTarget&) = delete;
    ViewportRenderTarget& operator=(const ViewportRenderTarget&) = delete;

    void create(VkPhysicalDevice physicalDevice, VkDevice device, VkExtent2D extent,
                VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
    void resize(VkExtent2D extent);
    void destroy() noexcept;

    [[nodiscard]] VkExtent2D extent() const noexcept { return extent_; }
    [[nodiscard]] const HdrBuffer& color() const noexcept { return color_; }
    [[nodiscard]] const DepthBuffer& depth() const noexcept { return depth_; }
    [[nodiscard]] VkDescriptorImageInfo colorDescriptor() const noexcept;

private:
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
    VkExtent2D extent_{};
    HdrBuffer color_{};
    DepthBuffer depth_{};
};

} // namespace Engine
