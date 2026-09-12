#pragma once

#include "Engine/Math/Math.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace Engine {

/** Shared CPU reference implementation of environment-map convolution.
 *
 * The input callback is deliberately independent of an HDR panorama.  It can
 * sample an equirectangular sky today and a captured probe cubemap after its
 * mip-zero texels are made available to the baker.  The GPU baker must retain
 * the same mip-to-roughness contract. */
class EnvironmentBaker final {
public:
    using RadianceSampler = std::function<Vec3(const Vec3& direction)>;

    /** Maps a cubemap mip to the PBR roughness it represents. */
    [[nodiscard]] static constexpr float roughnessForMip(const std::uint32_t mip,
                                                          const std::uint32_t mipLevels) noexcept {
        return mipLevels > 1 ? static_cast<float>(mip) / static_cast<float>(mipLevels - 1) : 0.0F;
    }

    [[nodiscard]] static std::vector<float> bakeCubemap(
        std::uint32_t faceSize, std::uint32_t mipLevels,
        const std::function<Vec3(const Vec3&, std::uint32_t, std::uint32_t)>& evaluator);

    /** GGX-convolves source into a complete mip chain. Mip zero has roughness
     * 0 and the final mip has roughness 1. */
    [[nodiscard]] static std::vector<float> bakePrefilteredCubemap(
        std::uint32_t faceSize, std::uint32_t mipLevels, const RadianceSampler& source);

    [[nodiscard]] static Vec3 diffuseIrradiance(const Vec3& normal, const RadianceSampler& source);

private:
    [[nodiscard]] static Vec3 prefilter(const Vec3& reflection, float roughness,
                                        const RadianceSampler& source);
};

} // namespace Engine
