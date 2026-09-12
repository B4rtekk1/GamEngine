#pragma once

#include "Engine/Renderer/Textures/Cubemap.h"
#include "Engine/Renderer/Textures/Texture2D.h"

#include <array>
#include <filesystem>

namespace Engine {

// Global lighting textures used by the forward PBR descriptor set. The
// procedural HDR fallback is convolved into irradiance and GGX-prefiltered
// maps at startup; an imported HDR environment can replace that source.
class ImageBasedLighting final {
public:
    void create(VkPhysicalDevice physicalDevice, VkDevice device, VkCommandPool commandPool,
                VkQueue queue, VmaAllocator allocator,
                const std::filesystem::path& equirectangularPath = {});
    void destroy() noexcept;

    [[nodiscard]] std::array<VkDescriptorImageInfo, 3> descriptors() const noexcept;

private:
    Cubemap environment_;
    Cubemap irradiance_;
    Cubemap prefiltered_;
    Texture2D brdfLut_;
};

} // namespace Engine
