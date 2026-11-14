#pragma once

#include "Engine/Core/PngWriter.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace Editor {

namespace detail {

[[nodiscard]] inline float halfToFloat(const std::uint16_t value) {
    const std::uint32_t sign = (value & 0x8000U) << 16U;
    std::uint32_t exponent = (value >> 10U) & 0x1FU;
    std::uint32_t mantissa = value & 0x3FFU;
    std::uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            exponent = 127 - 15 + 1;
            while ((mantissa & 0x400U) == 0) {
                mantissa <<= 1U;
                --exponent;
            }
            mantissa &= 0x3FFU;
            bits = sign | (exponent << 23U) | (mantissa << 13U);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7F800000U | (mantissa << 13U);
    } else {
        bits = sign | ((exponent + 127 - 15) << 23U) | (mantissa << 13U);
    }
    return std::bit_cast<float>(bits);
}

} // namespace detail

/** Tone-maps a lit half-float RGBA capture to sRGB and writes it as an (uncompressed) PNG. */
[[nodiscard]] inline bool writeHdrScreenshot(const std::filesystem::path& path, const std::uint32_t width,
                                             const std::uint32_t height, const std::vector<std::uint16_t>& rgbaHalf) {
    if (width == 0 || height == 0 || rgbaHalf.size() < std::size_t{width} * height * 4)
        return false;
    std::vector<std::uint8_t> rgb;
    rgb.reserve(std::size_t{width} * 3 * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            for (int channel = 0; channel < 3; ++channel) {
                const float linear = std::max(0.0F, detail::halfToFloat(rgbaHalf[(std::size_t{y} * width + x) * 4 + channel]));
                const float mapped = linear / (1.0F + linear); // Reinhard
                const float srgb = mapped <= 0.0031308F ? mapped * 12.92F : 1.055F * std::pow(mapped, 1.0F / 2.4F) - 0.055F;
                rgb.push_back(static_cast<std::uint8_t>(std::clamp(srgb, 0.0F, 1.0F) * 255.0F + 0.5F));
            }
        }
    }
    return Engine::writePngRgb8(path, width, height, rgb);
}

} // namespace Editor
