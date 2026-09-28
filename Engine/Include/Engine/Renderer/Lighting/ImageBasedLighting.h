#pragma once

#include "Engine/Math/Vec3.h"
#include "Engine/Renderer/Textures/Cubemap.h"
#include "Engine/Renderer/Textures/Texture2D.h"
#include "Engine/Renderer/RenderConfig.h"

#include <array>
#include <filesystem>
#include <optional>

namespace Engine {

// Global lighting textures used by the forward PBR descriptor set. The
// procedural HDR fallback is convolved into irradiance and GGX-prefiltered
// maps at startup; an imported HDR environment can replace that source.
class ImageBasedLighting final {
public:
    struct EnvironmentSun {
        // Points from the scene toward the bright region of the panorama.
        Vec3 direction;
        Vec3 color;
        float intensity = 0.0F;
    };

    void create(VkPhysicalDevice physicalDevice, VkDevice device, VkCommandPool commandPool,
                VkQueue queue, VmaAllocator allocator,
                const std::filesystem::path& equirectangularPath = {},
                const std::filesystem::path& libraryDirectory = {},
                IblQualitySettings quality = iblQualitySettings(IblQuality::High));
    void destroy() noexcept;
    void swap(ImageBasedLighting& other) noexcept;

    [[nodiscard]] std::array<VkDescriptorImageInfo, 3> descriptors() const noexcept;
    [[nodiscard]] VkDescriptorImageInfo environmentDescriptor() const noexcept;
    [[nodiscard]] VkImage environmentImage() const noexcept { return environment_.image(); }
    [[nodiscard]] std::uint32_t environmentSize() const noexcept { return environment_.faceSize(); }
    [[nodiscard]] std::uint32_t environmentMipLevels() const noexcept { return environment_.mipLevels(); }
    [[nodiscard]] std::uint32_t prefilteredMipLevels() const noexcept { return prefiltered_.mipLevels(); }
    [[nodiscard]] const std::optional<EnvironmentSun>& sun() const noexcept { return sun_; }

private:
    Cubemap environment_;
    Cubemap irradiance_;
    Cubemap prefiltered_;
    Texture2D brdfLut_;
    std::optional<EnvironmentSun> sun_;
};

} // namespace Engine
