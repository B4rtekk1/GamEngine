#include "Engine/Renderer/Lighting/ImageBasedLighting.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace Engine {
namespace {
constexpr std::array<std::array<std::uint8_t, 4>, 6> EnvironmentFaces{{
    {{68, 132, 205, 255}}, {{58, 110, 180, 255}}, {{105, 170, 225, 255}},
    {{12, 24, 55, 255}}, {{78, 142, 210, 255}}, {{52, 102, 170, 255}},
}};
}

void ImageBasedLighting::create(const VkPhysicalDevice physicalDevice, const VkDevice device,
                                const VkCommandPool commandPool, const VkQueue queue,
                                const VmaAllocator allocator) {
    destroy();
    // The diffuse convolution of a low-frequency procedural sky is its
    // hemispherical average. Keep it separate from the source cubemap so a
    // later HDR/capture baker can replace just this generation step.
    std::array<std::array<std::uint8_t, 4>, 6> irradianceFaces{};
    std::array<std::uint32_t, 3> sum{};
    for (const auto& face : EnvironmentFaces)
        for (std::uint32_t channel = 0; channel < 3; ++channel) sum[channel] += face[channel];
    for (auto& face : irradianceFaces) {
        for (std::uint32_t channel = 0; channel < 3; ++channel)
            face[channel] = static_cast<std::uint8_t>(sum[channel] / EnvironmentFaces.size());
        face[3] = 255;
    }

    environment_.create(physicalDevice, device, commandPool, queue, EnvironmentFaces);
    irradiance_.create(physicalDevice, device, commandPool, queue, irradianceFaces);
    // This is the GGX prefilter's bootstrap representation. The procedural
    // one-texel sky has no angular detail to blur; an HDR environment baker
    // will replace it with a roughness-indexed mip chain without changing the
    // material-side SampleLevel contract.
    prefiltered_.create(physicalDevice, device, commandPool, queue, EnvironmentFaces);

    constexpr std::uint32_t size = 128;
    std::vector<std::uint8_t> pixels(size * size * 4);
    for (std::uint32_t y = 0; y < size; ++y) {
        const float roughness = (static_cast<float>(y) + 0.5F) / static_cast<float>(size);
        for (std::uint32_t x = 0; x < size; ++x) {
            const float nDotV = (static_cast<float>(x) + 0.5F) / static_cast<float>(size);
            // UE4 split-sum BRDF approximation (A/B), stored in a linear LUT.
            const float r0 = roughness * -1.0F + 1.0F;
            const float a004 = std::min(r0 * r0, std::exp2(-9.28F * nDotV)) * r0 + roughness;
            const float a = -1.04F * a004 + 1.04F;
            const float b = 1.04F * a004 - 0.04F;
            const auto index = (y * size + x) * 4;
            pixels[index] = static_cast<std::uint8_t>(std::clamp(a, 0.0F, 1.0F) * 255.0F + 0.5F);
            pixels[index + 1] = static_cast<std::uint8_t>(std::clamp(b, 0.0F, 1.0F) * 255.0F + 0.5F);
            pixels[index + 2] = 0;
            pixels[index + 3] = 255;
        }
    }
    brdfLut_.create(physicalDevice, device, commandPool, queue, size, size, pixels,
                    TextureColorSpace::Linear, false, allocator);
}

void ImageBasedLighting::destroy() noexcept {
    brdfLut_.destroy();
    prefiltered_.destroy();
    irradiance_.destroy();
    environment_.destroy();
}

std::array<VkDescriptorImageInfo, 3> ImageBasedLighting::descriptors() const noexcept {
    return {{{irradiance_.sampler(), irradiance_.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
             {prefiltered_.sampler(), prefiltered_.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
             {brdfLut_.sampler(), brdfLut_.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}};
}

} // namespace Engine
