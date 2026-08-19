#include "Engine/Renderer/Vulkan/ViewportRenderTarget.h"

#include <stdexcept>

namespace Engine {

void ViewportRenderTarget::create(const VkPhysicalDevice physicalDevice, const VkDevice device,
                                  const VkExtent2D extent, const VkSampleCountFlagBits samples) {
    if (physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE ||
        extent.width == 0 || extent.height == 0) {
        throw std::invalid_argument("ViewportRenderTarget requires a device and non-zero extent");
    }
    destroy();
    physicalDevice_ = physicalDevice;
    device_ = device;
    samples_ = samples;
    extent_ = extent;
    try {
        color_.create(physicalDevice_, device_, extent_);
        msaaColor_.initialize(physicalDevice_, device_, samples_);
        // MsaaResources may fall back (for example, from 4x to 2x) when the
        // selected GPU cannot multisample both color and depth at the
        // requested rate.  The depth image must use that *effective* rate as
        // well; otherwise the Scene View framebuffer contains attachments
        // with different sample counts.  Some drivers report that only at
        // submission time as VK_ERROR_DEVICE_LOST.
        samples_ = msaaColor_.sampleCount();
        msaaColor_.create(extent_, HdrBuffer::Format);
        depth_.initialize(physicalDevice_, device_);
        depth_.create(extent_, samples_);
    } catch (...) {
        destroy();
        throw;
    }
}

void ViewportRenderTarget::resize(const VkExtent2D extent) {
    if (extent.width == 0 || extent.height == 0) return;
    if (extent.width == extent_.width && extent.height == extent_.height) return;
    if (device_ == VK_NULL_HANDLE) throw std::logic_error("ViewportRenderTarget is not initialized");
    create(physicalDevice_, device_, extent, samples_);
}

void ViewportRenderTarget::destroy() noexcept {
    depth_.destroy();
    msaaColor_.destroy();
    color_.destroy();
    physicalDevice_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    extent_ = {};
}

VkDescriptorImageInfo ViewportRenderTarget::colorDescriptor() const noexcept {
    return {color_.sampler(), color_.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

} // namespace Engine
