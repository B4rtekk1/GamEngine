#include "Engine/Renderer/Lighting/ImageBasedLighting.h"

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
constexpr float Pi = 3.14159265358979323846F;
constexpr std::uint32_t EnvironmentSize = 512;
constexpr std::uint32_t IrradianceSize = 32;
constexpr std::uint32_t PrefilterSize = 256;

struct Vec3 { float x, y, z; };
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 normalize(Vec3 v) { const float length = std::sqrt(std::max(dot(v, v), 1e-12F)); return v * (1.0F / length); }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
Vec3 reflect(Vec3 incident, Vec3 normal) { return incident - normal * (2.0F * dot(normal, incident)); }

Vec3 faceDirection(std::uint32_t face, float u, float v) {
    // Matches Vulkan's conventional cube-face orientation.
    switch (face) {
        case 0: return normalize({ 1.0F, -v, -u});
        case 1: return normalize({-1.0F, -v,  u});
        case 2: return normalize({ u,  1.0F,  v});
        case 3: return normalize({ u, -1.0F, -v});
        case 4: return normalize({ u, -v,  1.0F});
        default:return normalize({-u, -v, -1.0F});
    }
}

// Procedural fallback is an HDR *directional* source, not six flat colours.
// Replacing radiance() with HDR/EXR cubemap sampling keeps the baker unchanged.
Vec3 proceduralRadiance(Vec3 direction) {
    const float horizon = std::clamp(direction.y * 0.5F + 0.5F, 0.0F, 1.0F);
    Vec3 sky = Vec3{0.035F, 0.055F, 0.12F} * (1.0F - horizon) + Vec3{0.22F, 0.48F, 0.95F} * horizon;
    const Vec3 sun = normalize({0.35F, 0.82F, -0.45F});
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
        const float longitude = std::atan2(direction.z, direction.x);
        const float u = longitude * (0.5F / Pi) + 0.5F;
        const float v = std::acos(std::clamp(direction.y, -1.0F, 1.0F)) / Pi;
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

float radicalInverseVdC(std::uint32_t bits) {
    bits = (bits << 16U) | (bits >> 16U);
    bits = ((bits & 0x55555555U) << 1U) | ((bits & 0xAAAAAAAAU) >> 1U);
    bits = ((bits & 0x33333333U) << 2U) | ((bits & 0xCCCCCCCCU) >> 2U);
    bits = ((bits & 0x0F0F0F0FU) << 4U) | ((bits & 0xF0F0F0F0U) >> 4U);
    bits = ((bits & 0x00FF00FFU) << 8U) | ((bits & 0xFF00FF00U) >> 8U);
    return static_cast<float>(bits) * 2.3283064365386963e-10F;
}

Vec3 tangentToWorld(Vec3 local, Vec3 normal) {
    const Vec3 up = std::abs(normal.y) < 0.999F ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    const Vec3 tangent = normalize(cross(up, normal));
    const Vec3 bitangent = cross(normal, tangent);
    return normalize(tangent * local.x + bitangent * local.y + normal * local.z);
}

Vec3 diffuseIrradiance(Vec3 normal, const EnvironmentSource& source) {
    constexpr std::uint32_t Samples = 64;
    Vec3 result{};
    for (std::uint32_t i = 0; i < Samples; ++i) {
        const float xi1 = static_cast<float>(i) / Samples;
        const float xi2 = radicalInverseVdC(i);
        const float r = std::sqrt(xi1);
        const Vec3 local{r * std::cos(2.0F * Pi * xi2), r * std::sin(2.0F * Pi * xi2), std::sqrt(1.0F - xi1)};
        result = result + source.sample(tangentToWorld(local, normal));
    }
    return result * (Pi / static_cast<float>(Samples));
}

Vec3 importanceSampleGgx(float xi1, float xi2, float roughness, Vec3 normal) {
    const float a = roughness * roughness;
    const float phi = 2.0F * Pi * xi1;
    const float cosTheta = std::sqrt((1.0F - xi2) / (1.0F + (a * a - 1.0F) * xi2));
    const float sinTheta = std::sqrt(std::max(1.0F - cosTheta * cosTheta, 0.0F));
    return tangentToWorld({std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta}, normal);
}

Vec3 prefilter(Vec3 reflection, float roughness, const EnvironmentSource& source) {
    if (roughness <= 0.001F) return source.sample(reflection);
    constexpr std::uint32_t Samples = 128;
    Vec3 result{}; float weight = 0.0F;
    // For the split-sum prefilter V=N. Every mip therefore contains the GGX
    // integral appropriate to its roughness rather than a box-filtered copy.
    for (std::uint32_t i = 0; i < Samples; ++i) {
        const Vec3 halfVector = importanceSampleGgx(static_cast<float>(i) / Samples, radicalInverseVdC(i), roughness, reflection);
        const Vec3 light = normalize(reflect(reflection * -1.0F, halfVector));
        const float nDotL = std::max(dot(reflection, light), 0.0F);
        if (nDotL > 0.0F) { result = result + source.sample(light) * nDotL; weight += nDotL; }
    }
    return weight > 0.0F ? result * (1.0F / weight) : source.sample(reflection);
}

template <typename Evaluator>
std::vector<float> bakeCubemap(std::uint32_t size, std::uint32_t mipLevels, Evaluator evaluator) {
    std::size_t texels = 0;
    for (std::uint32_t mip = 0; mip < mipLevels; ++mip) { const auto side = std::max(1U, size >> mip); texels += static_cast<std::size_t>(6) * side * side; }
    std::vector<float> pixels(texels * 4); std::size_t index = 0;
    for (std::uint32_t mip = 0; mip < mipLevels; ++mip) {
        const auto side = std::max(1U, size >> mip);
        for (std::uint32_t face = 0; face < 6; ++face)
            for (std::uint32_t y = 0; y < side; ++y)
                for (std::uint32_t x = 0; x < side; ++x) {
                    const Vec3 direction = faceDirection(face, 2.0F * (static_cast<float>(x) + 0.5F) / side - 1.0F, 2.0F * (static_cast<float>(y) + 0.5F) / side - 1.0F);
                    const Vec3 color = evaluator(direction, mip, mipLevels);
                    pixels[index++] = color.x; pixels[index++] = color.y; pixels[index++] = color.z; pixels[index++] = 1.0F;
                }
    }
    return pixels;
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
        bakeCubemap(EnvironmentSize, 1, [&source](Vec3 direction, std::uint32_t, std::uint32_t) { return source.sample(direction); }));
    replacement.irradiance_.createHdr(physicalDevice, device, commandPool, queue, IrradianceSize, 1,
        bakeCubemap(IrradianceSize, 1, [&source](Vec3 direction, std::uint32_t, std::uint32_t) { return diffuseIrradiance(direction, source); }));
    replacement.prefiltered_.createHdr(physicalDevice, device, commandPool, queue, PrefilterSize, prefilterMips,
        bakeCubemap(PrefilterSize, prefilterMips, [&source](Vec3 direction, std::uint32_t mip, std::uint32_t mipCount) {
            return prefilter(direction, static_cast<float>(mip) / static_cast<float>(mipCount - 1), source);
        }));

    constexpr std::uint32_t size = 128;
    std::vector<std::uint8_t> pixels(size * size * 4);
    for (std::uint32_t y = 0; y < size; ++y) {
        const float roughness = (static_cast<float>(y) + 0.5F) / static_cast<float>(size);
        for (std::uint32_t x = 0; x < size; ++x) {
            const float nDotV = (static_cast<float>(x) + 0.5F) / static_cast<float>(size);
            const float r0 = 1.0F - roughness;
            const float a004 = std::min(r0 * r0, std::exp2(-9.28F * nDotV)) * r0 + roughness;
            const float a = -1.04F * a004 + 1.04F; const float b = 1.04F * a004 - 0.04F;
            const auto index = (y * size + x) * 4;
            pixels[index] = static_cast<std::uint8_t>(std::clamp(a, 0.0F, 1.0F) * 255.0F + 0.5F);
            pixels[index + 1] = static_cast<std::uint8_t>(std::clamp(b, 0.0F, 1.0F) * 255.0F + 0.5F);
            pixels[index + 3] = 255;
        }
    }
    replacement.brdfLut_.create(physicalDevice, device, commandPool, queue, size, size, pixels, TextureColorSpace::Linear, false, allocator);
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
