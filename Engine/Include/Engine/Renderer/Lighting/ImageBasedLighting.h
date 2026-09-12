#pragma once

#include "Engine/Renderer/Textures/Cubemap.h"
#include "Engine/Renderer/Textures/Texture2D.h"

#include <array>

namespace Engine {

// Global lighting textures used by the forward PBR descriptor set.  The
// current procedural sky is deliberately also the source of these maps, so
// IBL is useful before an HDR environment-asset import path is added.
class ImageBasedLighting final {
public:
    void create(VkPhysicalDevice physicalDevice, VkDevice device, VkCommandPool commandPool,
                VkQueue queue, VmaAllocator allocator);
    void destroy() noexcept;

    [[nodiscard]] std::array<VkDescriptorImageInfo, 3> descriptors() const noexcept;

private:
    Cubemap environment_;
    Cubemap irradiance_;
    Cubemap prefiltered_;
    Texture2D brdfLut_;
};

} // namespace Engine
