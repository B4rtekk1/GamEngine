#include "Engine/Renderer/Vulkan/ViewportRenderTarget.h"

#include <stdexcept>

namespace Engine {
    void ViewportRenderTarget::create(const VkPhysicalDevice physicalDevice, const VkDevice device,
                                      const VkExtent2D extent, const VmaAllocator allocator) {
        if (physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE ||
            extent.width == 0 || extent.height == 0 || allocator == VK_NULL_HANDLE) {
            throw std::invalid_argument("ViewportRenderTarget requires a device and non-zero extent");
        }
        destroy();
        physicalDevice_ = physicalDevice;
        device_ = device;
        allocator_ = allocator;
        extent_ = extent;
        try {
            color_.create(physicalDevice_, device_, extent_, allocator_);
        } catch (...) {
            destroy();
            throw;
        }
    }

    void ViewportRenderTarget::resize(const VkExtent2D extent) {
        if (extent.width == 0 || extent.height == 0) {
            return;
        }
        if (extent.width == extent_.width && extent.height == extent_.height) {
            return;
        }
        if (device_ == VK_NULL_HANDLE) {
            throw std::logic_error("ViewportRenderTarget is not initialized");
        }
        create(physicalDevice_, device_, extent, allocator_);
    }

    void ViewportRenderTarget::destroy() noexcept {
        color_.destroy();
        physicalDevice_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
        allocator_ = VK_NULL_HANDLE;
        extent_ = {};
    }

    VkDescriptorImageInfo ViewportRenderTarget::colorDescriptor() const noexcept {
        return {color_.sampler(), color_.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
} // namespace Engine
