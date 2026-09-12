#include "Engine/Renderer/Lighting/EnvironmentBaker.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Engine {
namespace {
float radicalInverseVdC(std::uint32_t bits) {
    bits = (bits << 16U) | (bits >> 16U);
    bits = ((bits & 0x55555555U) << 1U) | ((bits & 0xAAAAAAAAU) >> 1U);
    bits = ((bits & 0x33333333U) << 2U) | ((bits & 0xCCCCCCCCU) >> 2U);
    bits = ((bits & 0x0F0F0F0FU) << 4U) | ((bits & 0xF0F0F0F0U) >> 4U);
    bits = ((bits & 0x00FF00FFU) << 8U) | ((bits & 0xFF00FF00U) >> 8U);
    return static_cast<float>(bits) * 2.3283064365386963e-10F;
}

Vec3 tangentToWorld(const Vec3 local, const Vec3 normal) {
    const Vec3 up = std::abs(normal.y()) < 0.999F ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    const Vec3 tangent = cross(up, normal).normalized();
    const Vec3 bitangent = cross(normal, tangent);
    return (tangent * local.x() + bitangent * local.y() + normal * local.z()).normalized();
}

Vec3 importanceSampleGgx(const float xi1, const float xi2, const float roughness, const Vec3 normal) {
    const float a = roughness * roughness;
    const float phi = Tau * xi1;
    const float cosTheta = std::sqrt((1.0F - xi2) / (1.0F + (a * a - 1.0F) * xi2));
    const float sinTheta = std::sqrt(std::max(1.0F - cosTheta * cosTheta, 0.0F));
    return tangentToWorld({std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta}, normal);
}

Vec3 faceDirection(const std::uint32_t face, const float u, const float v) {
    switch (face) {
        case 0: return Vec3{1.0F, -v, -u}.normalized();
        case 1: return Vec3{-1.0F, -v, u}.normalized();
        case 2: return Vec3{u, 1.0F, v}.normalized();
        case 3: return Vec3{u, -1.0F, -v}.normalized();
        case 4: return Vec3{u, -v, 1.0F}.normalized();
        default: return Vec3{-u, -v, -1.0F}.normalized();
    }
}
} // namespace

Vec3 EnvironmentBaker::diffuseIrradiance(const Vec3& normal, const RadianceSampler& source) {
    constexpr std::uint32_t Samples = 64;
    Vec3 result{};
    for (std::uint32_t i = 0; i < Samples; ++i) {
        const float xi1 = static_cast<float>(i) / Samples;
        const float xi2 = radicalInverseVdC(i);
        const float radius = std::sqrt(xi1);
        const Vec3 local{radius * std::cos(Tau * xi2), radius * std::sin(Tau * xi2),
                         std::sqrt(1.0F - xi1)};
        result += source(tangentToWorld(local, normal));
    }
    return result * (Pi / static_cast<float>(Samples));
}

Vec3 EnvironmentBaker::prefilter(const Vec3& reflection, const float roughness,
                                  const RadianceSampler& source) {
    if (roughness <= 0.001F) return source(reflection);
    constexpr std::uint32_t Samples = 128;
    Vec3 result{};
    float weight = 0.0F;
    for (std::uint32_t i = 0; i < Samples; ++i) {
        const Vec3 halfVector = importanceSampleGgx(static_cast<float>(i) / Samples,
                                                     radicalInverseVdC(i), roughness, reflection);
        const Vec3 light = (-reflection + halfVector * (2.0F * dot(reflection, halfVector))).normalized();
        const float nDotL = std::max(dot(reflection, light), 0.0F);
        if (nDotL > 0.0F) {
            result += source(light) * nDotL;
            weight += nDotL;
        }
    }
    return weight > 0.0F ? result * (1.0F / weight) : source(reflection);
}

std::vector<float> EnvironmentBaker::bakeCubemap(
    const std::uint32_t faceSize, const std::uint32_t mipLevels,
    const std::function<Vec3(const Vec3&, std::uint32_t, std::uint32_t)>& evaluator) {
    if (faceSize == 0 || mipLevels == 0) throw std::invalid_argument("Environment baker requires at least one mip");
    std::size_t texels = 0;
    for (std::uint32_t mip = 0; mip < mipLevels; ++mip) {
        const auto side = std::max(1U, faceSize >> mip);
        texels += static_cast<std::size_t>(6) * side * side;
    }
    std::vector<float> pixels(texels * 4);
    std::size_t index = 0;
    for (std::uint32_t mip = 0; mip < mipLevels; ++mip) {
        const auto side = std::max(1U, faceSize >> mip);
        for (std::uint32_t face = 0; face < 6; ++face)
            for (std::uint32_t y = 0; y < side; ++y)
                for (std::uint32_t x = 0; x < side; ++x) {
                    const Vec3 direction = faceDirection(face, 2.0F * (static_cast<float>(x) + 0.5F) / side - 1.0F,
                                                          2.0F * (static_cast<float>(y) + 0.5F) / side - 1.0F);
                    const Vec3 color = evaluator(direction, mip, mipLevels);
                    pixels[index++] = color.x(); pixels[index++] = color.y(); pixels[index++] = color.z(); pixels[index++] = 1.0F;
                }
    }
    return pixels;
}

std::vector<float> EnvironmentBaker::bakePrefilteredCubemap(
    const std::uint32_t faceSize, const std::uint32_t mipLevels, const RadianceSampler& source) {
    return bakeCubemap(faceSize, mipLevels, [&source](const Vec3& direction, const std::uint32_t mip,
                                                       const std::uint32_t count) {
        const float roughness = roughnessForMip(mip, count);
        return prefilter(direction, roughness, source);
    });
}
} // namespace Engine
