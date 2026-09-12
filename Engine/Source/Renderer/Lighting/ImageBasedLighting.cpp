#include "Engine/Renderer/Lighting/ImageBasedLighting.h"
#include "Engine/Renderer/Lighting/EnvironmentBaker.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <vector>

#include <stb_image.h>
#include <tinyexr.h>

namespace Engine {
namespace {
constexpr std::uint32_t EnvironmentSize = 512;
constexpr std::uint32_t IrradianceSize = 32;
constexpr std::uint32_t PrefilterSize = 256;
constexpr std::uint32_t BrdfLutSize = 256;
constexpr std::uint32_t BrdfLutSamples = 1024;

float radicalInverseVdC(std::uint32_t bits) noexcept {
    bits = (bits << 16U) | (bits >> 16U);
    bits = ((bits & 0x55555555U) << 1U) | ((bits & 0xAAAAAAAAU) >> 1U);
    bits = ((bits & 0x33333333U) << 2U) | ((bits & 0xCCCCCCCCU) >> 2U);
    bits = ((bits & 0x0F0F0F0FU) << 4U) | ((bits & 0xF0F0F0F0U) >> 4U);
    bits = ((bits & 0x00FF00FFU) << 8U) | ((bits & 0xFF00FF00U) >> 8U);
    return static_cast<float>(bits) * 2.3283064365386963e-10F;
}

Vec3 importanceSampleGgx(const float xi1, const float xi2, const float roughness) {
    const float alpha = roughness * roughness;
    const float phi = 2.0F * Pi * xi1;
    const float cosTheta = std::sqrt((1.0F - xi2) / (1.0F + (alpha * alpha - 1.0F) * xi2));
    const float sinTheta = std::sqrt(std::max(0.0F, 1.0F - cosTheta * cosTheta));
    return {std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta};
}

float geometrySchlickGgx(const float nDotX, const float roughness) noexcept {
    const float k = roughness * roughness * 0.5F;
    return nDotX / (nDotX * (1.0F - k) + k);
}

Vec2 integrateBrdf(const float nDotV, const float roughness) {
    const Vec3 view{std::sqrt(std::max(0.0F, 1.0F - nDotV * nDotV)), 0.0F, nDotV};
    float a = 0.0F;
    float b = 0.0F;
    for (std::uint32_t sample = 0; sample < BrdfLutSamples; ++sample) {
        const float xi1 = static_cast<float>(sample) / static_cast<float>(BrdfLutSamples);
        const Vec3 halfVector = importanceSampleGgx(xi1, radicalInverseVdC(sample), roughness);
        const Vec3 light = (halfVector * (2.0F * dot(view, halfVector)) - view).normalized();
        const float nDotL = std::max(light.z(), 0.0F);
        const float nDotH = std::max(halfVector.z(), 0.0F);
        const float vDotH = std::max(dot(view, halfVector), 0.0F);
        if (nDotL <= 0.0F) continue;
        const float visibility = geometrySchlickGgx(nDotV, roughness) *
                                 geometrySchlickGgx(nDotL, roughness) * vDotH /
                                 std::max(nDotH * nDotV, 1.0e-5F);
        const float fresnel = std::pow(1.0F - vDotH, 5.0F);
        a += (1.0F - fresnel) * visibility;
        b += fresnel * visibility;
    }
    return {a / static_cast<float>(BrdfLutSamples), b / static_cast<float>(BrdfLutSamples)};
}

std::uint16_t floatToHalf(const float value) noexcept {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign = (bits >> 16U) & 0x8000U;
    const int exponent = static_cast<int>((bits >> 23U) & 0xFFU) - 127 + 15;
    std::uint32_t mantissa = bits & 0x007FFFFFU;
    if (exponent <= 0) {
        if (exponent < -10) return static_cast<std::uint16_t>(sign);
        mantissa = (mantissa | 0x00800000U) >> static_cast<std::uint32_t>(1 - exponent);
        return static_cast<std::uint16_t>(sign | ((mantissa + 0x1000U) >> 13U));
    }
    if (exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7C00U);
    return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exponent) << 10U) |
                                      ((mantissa + 0x1000U) >> 13U));
}

// Procedural fallback is an HDR *directional* source, not six flat colours.
// Replacing radiance() with HDR/EXR cubemap sampling keeps the baker unchanged.
Vec3 proceduralRadiance(Vec3 direction) {
    const float horizon = std::clamp(direction.y() * 0.5F + 0.5F, 0.0F, 1.0F);
    Vec3 sky = Vec3{0.035F, 0.055F, 0.12F} * (1.0F - horizon) + Vec3{0.22F, 0.48F, 0.95F} * horizon;
    const Vec3 sun = Vec3{0.35F, 0.82F, -0.45F}.normalized();
    const float sunDisk = std::pow(std::max(dot(direction, sun), 0.0F), 2048.0F);
    return sky + Vec3{18.0F, 14.0F, 8.0F} * sunDisk;
}

struct EnvironmentSource {
    int width = 0;
    int height = 0;
    std::vector<float> pixels;

    [[nodiscard]] bool loaded() const noexcept { return width > 0 && height > 0; }

    [[nodiscard]] Vec3 sample(Vec3 direction) const {
        if (!loaded()) return proceduralRadiance(direction);
        const float longitude = std::atan2(direction.z(), direction.x());
        const float u = longitude * (0.5F / Pi) + 0.5F;
        const float v = std::acos(std::clamp(direction.y(), -1.0F, 1.0F)) / Pi;
        const float x = u * static_cast<float>(width) - 0.5F;
        const float y = v * static_cast<float>(height) - 0.5F;
        const int x0 = (static_cast<int>(std::floor(x)) % width + width) % width;
        const int x1 = (x0 + 1) % width;
        const int y0 = std::clamp(static_cast<int>(std::floor(y)), 0, height - 1);
        const int y1 = std::min(y0 + 1, height - 1);
        const float tx = x - std::floor(x); const float ty = y - std::floor(y);
        const auto pixel = [this](int px, int py) {
            const auto index = (static_cast<std::size_t>(py) * width + px) * 4;
            return Vec3{pixels[index], pixels[index + 1], pixels[index + 2]};
        };
        const Vec3 a = pixel(x0, y0) * (1.0F - tx) + pixel(x1, y0) * tx;
        const Vec3 b = pixel(x0, y1) * (1.0F - tx) + pixel(x1, y1) * tx;
        return a * (1.0F - ty) + b * ty;
    }
};

EnvironmentSource loadEquirectangular(const std::filesystem::path& path) {
    if (path.empty()) return {};
    const auto extension = path.extension().string();
    if (extension == ".exr" || extension == ".EXR") {
        float* decoded = nullptr;
        int width = 0; int height = 0;
        const char* error = nullptr;
        const std::string nativePath = path.string();
        if (LoadEXR(&decoded, &width, &height, nativePath.c_str(), &error) != TINYEXR_SUCCESS ||
            decoded == nullptr || width <= 0 || height <= 0) {
            std::cerr << "[IBL] Could not load EXR panorama '" << path.string() << "': "
                      << (error != nullptr ? error : "invalid EXR") << "; using procedural fallback.\n";
            if (error != nullptr) FreeEXRErrorMessage(error);
            if (decoded != nullptr) free(decoded);
            return {};
        }
        EnvironmentSource source; source.width = width; source.height = height;
        source.pixels.assign(decoded, decoded + static_cast<std::size_t>(width) * height * 4);
        free(decoded);
        return source;
    }
    int width = 0; int height = 0; int channels = 0;
    const std::string nativePath = path.string();
    float* const decoded = stbi_loadf(nativePath.c_str(), &width, &height, &channels, 4);
    if (decoded == nullptr || width <= 0 || height <= 0) {
        std::cerr << "[IBL] Could not load HDR panorama '" << path.string() << "': "
                  << stbi_failure_reason() << "; using procedural fallback.\n";
        if (decoded != nullptr) stbi_image_free(decoded);
        return {};
    }
    EnvironmentSource source; source.width = width; source.height = height;
    source.pixels.assign(decoded, decoded + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(decoded);
    return source;
}

}

void ImageBasedLighting::create(VkPhysicalDevice physicalDevice, VkDevice device, VkCommandPool commandPool,
                                VkQueue queue, VmaAllocator allocator,
                                const std::filesystem::path& equirectangularPath) {
    const EnvironmentSource source = loadEquirectangular(equirectangularPath);
    // Build every resource away from the live set. A decode, allocation or
    // upload failure must leave the active descriptor targets intact.
    ImageBasedLighting replacement;
    const auto prefilterMips = std::bit_width(PrefilterSize);
    replacement.environment_.createHdr(physicalDevice, device, commandPool, queue, EnvironmentSize, 1,
        EnvironmentBaker::bakeCubemap(EnvironmentSize, 1,
            [&source](const Vec3& direction, std::uint32_t, std::uint32_t) { return source.sample(direction); }));
    replacement.irradiance_.createHdr(physicalDevice, device, commandPool, queue, IrradianceSize, 1,
        EnvironmentBaker::bakeCubemap(IrradianceSize, 1, [&source](const Vec3& direction, std::uint32_t, std::uint32_t) {
            return EnvironmentBaker::diffuseIrradiance(direction,
                [&source](const Vec3& sampleDirection) { return source.sample(sampleDirection); });
        }));
    replacement.prefiltered_.createHdr(physicalDevice, device, commandPool, queue, PrefilterSize, prefilterMips,
        EnvironmentBaker::bakePrefilteredCubemap(PrefilterSize, prefilterMips,
            [&source](const Vec3& direction) { return source.sample(direction); }));

    std::vector<std::uint16_t> pixels(BrdfLutSize * BrdfLutSize * 2);
    for (std::uint32_t y = 0; y < BrdfLutSize; ++y) {
        const float roughness = (static_cast<float>(y) + 0.5F) / static_cast<float>(BrdfLutSize);
        for (std::uint32_t x = 0; x < BrdfLutSize; ++x) {
            const float nDotV = (static_cast<float>(x) + 0.5F) / static_cast<float>(BrdfLutSize);
            const Vec2 value = integrateBrdf(nDotV, roughness);
            const auto index = (y * BrdfLutSize + x) * 2;
            pixels[index] = floatToHalf(value.x());
            pixels[index + 1] = floatToHalf(value.y());
        }
    }
    const auto bytes = std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(pixels.data()),
                                                      pixels.size() * sizeof(std::uint16_t)};
    replacement.brdfLut_.create(physicalDevice, device, commandPool, queue, BrdfLutSize, BrdfLutSize, bytes,
                                TextureColorSpace::Linear, false, allocator, TexturePixelFormat::RG16F);
    swap(replacement);
}

void ImageBasedLighting::destroy() noexcept { brdfLut_.destroy(); prefiltered_.destroy(); irradiance_.destroy(); environment_.destroy(); }

void ImageBasedLighting::swap(ImageBasedLighting& other) noexcept {
    using std::swap;
    swap(environment_, other.environment_);
    swap(irradiance_, other.irradiance_);
    swap(prefiltered_, other.prefiltered_);
    swap(brdfLut_, other.brdfLut_);
}

std::array<VkDescriptorImageInfo, 3> ImageBasedLighting::descriptors() const noexcept {
    return {{{irradiance_.sampler(), irradiance_.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
             {prefiltered_.sampler(), prefiltered_.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
             {brdfLut_.sampler(), brdfLut_.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}};
}

VkDescriptorImageInfo ImageBasedLighting::environmentDescriptor() const noexcept {
    return {environment_.sampler(), environment_.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}
} // namespace Engine
