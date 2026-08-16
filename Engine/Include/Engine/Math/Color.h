/**
 * @file Color.h
 * @brief RGBA color representation and color conversion utilities.
 */
#pragma once

#include "Engine/Math/Vec4.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace Engine {
    /**
     * @brief Represents an RGBA color using normalized floating-point channels.
     *
     * Each channel is normally expected to be in the range [0, 1]. The class
     * also supports conversion to and from 8-bit RGBA and Vulkan's packed
     * A2B10G10R10 format.
     */
    class Color final {
    public:
        /** @brief Constructs opaque black. */
        constexpr Color() noexcept = default;

        /**
         * @brief Constructs a color from normalized channels.
         * @param red Red channel in normalized floating-point form.
         * @param green Green channel in normalized floating-point form.
         * @param blue Blue channel in normalized floating-point form.
         * @param alpha Alpha channel in normalized floating-point form.
         */
        constexpr Color(float red, float green, float blue, float alpha = 1.0) noexcept : r_(red), g_(green), b_(blue), a_(alpha) {}

        /**
         * @brief Creates an opaque color from RGB channels.
         * @param red Red channel in the range [0, 1].
         * @param green Green channel in the range [0, 1].
         * @param blue Blue channel in the range [0, 1].
         * @return A color with alpha equal to 1.
         */
        [[nodiscard]] static constexpr Color from_rgb(float red, float green, float blue) noexcept {
            return {red, green, blue, 1.0f};
        }

        /**
         * @brief Creates a color from RGBA channels.
         * @param red Red channel in the range [0, 1].
         * @param green Green channel in the range [0, 1].
         * @param blue Blue channel in the range [0, 1].
         * @param alpha Alpha channel in the range [0, 1].
         * @return Constructed color.
         */
        [[nodiscard]] static constexpr Color from_rgba(float red, float green, float blue, float alpha) noexcept {
            return {red, green, blue, alpha};
        }

        /**
         * @brief Creates a color from packed 8-bit RGBA channels.
         * @param value Packed value in the format 0xRRGGBBAA.
         * @return Color with channels normalized to [0, 1].
         */
        [[nodiscard]] static constexpr Color from_rgba8(std::uint32_t value) noexcept {
            return {
                static_cast<float>((value >> 24) & 0xffu) / 255.0f,
                static_cast<float>((value >> 16) & 0xffu) / 255.0f,
                static_cast<float>((value >> 8) & 0xffu) / 255.0f,
                static_cast<float>((value) & 0xffu) / 255.0f,
            };
        }

        /**
         * @brief Creates a color from Vulkan's packed A2B10G10R10 format.
         *
         * The layout is R in bits 0..9, G in bits 10..19, B in bits 20..29
         * and A in bits 30..31.
         *
         * @param value Packed `VK_FORMAT_A2B10G10R10_UNORM_PACK32` value.
         * @return Color with normalized channels.
         */
        [[nodiscard]] static constexpr Color from_a2b10g10r10(std::uint32_t value) noexcept {
            return {
                static_cast<float>(value & 0x3ffu) / 1023.0f,
                static_cast<float>((value >> 10u) & 0x3ffu) / 1023.0f,
                static_cast<float>((value >> 20u) & 0x3ffu) / 1023.0f,
                static_cast<float>((value >> 30u) & 0x3u) / 3.0f,
            };
        }

        /**
         * @brief Creates a color from individual 10-bit RGB channels.
         * @param red Red channel. Only the lowest 10 bits are used.
         * @param green Green channel. Only the lowest 10 bits are used.
         * @param blue Blue channel. Only the lowest 10 bits are used.
         * @param alpha Two-bit alpha channel. Only the lowest 2 bits are used.
         * @return Color with normalized channels.
         */
        [[nodiscard]] static constexpr Color from_rgb10(std::uint16_t red,
                                                         std::uint16_t green,
                                                         std::uint16_t blue,
                                                         std::uint8_t alpha = 3) noexcept {
            return {
                static_cast<float>(red & 0x3ffu) / 1023.0f,
                static_cast<float>(green & 0x3ffu) / 1023.0f,
                static_cast<float>(blue & 0x3ffu) / 1023.0f,
                static_cast<float>(alpha & 0x3u) / 3.0f,
            };
        }

        /**
         * @brief Packs the color into Vulkan's A2B10G10R10 format.
         * @return Packed and clamped color value.
         * @note Channels are quantized to 10-bit RGB and 2-bit alpha values.
         */
        [[nodiscard]] std::uint32_t to_a2b10g10r10() const noexcept {
            const Color normalized = clamped();
            const auto quantize = [](const float value, const float maximum) {
                return static_cast<std::uint32_t>(std::lround(value * maximum));
            };
            return quantize(normalized.r_, 1023.0f) |
                   (quantize(normalized.g_, 1023.0f) << 10u) |
                   (quantize(normalized.b_, 1023.0f) << 20u) |
                   (quantize(normalized.a_, 3.0f) << 30u);
        }

        /**
         * @brief Parses a hexadecimal color string.
         * @param hex String in `RRGGBB`, `RRGGBBAA`, `#RRGGBB` or `#RRGGBBAA` form.
         * @return Parsed color.
         * @throws std::invalid_argument If the length or a hexadecimal digit is invalid.
         */
        [[nodiscard]] static Color from_hex(std::string_view hex) {
            if (!hex.empty() && hex.front() == '#') hex.remove_prefix(1);
            if (hex.size() != 6 && hex.size() != 8) {
                throw std::invalid_argument("Color hex value must contain 6 or 8 digits");
            }

            const auto digit = [](char c) -> std::uint32_t {
                if (c >= '0' && c <= '9') return static_cast<std::uint32_t>(c - '0');
                if (c >= 'a' && c <= 'f') return static_cast<std::uint32_t>(c - 'a' + 10);
                if (c >= 'A' && c <= 'F') return static_cast<std::uint32_t>(c - 'A' + 10);
                throw std::invalid_argument("Color hex value contains invalid character");
            };
            const auto byte = [&](std::size_t offset) {
                return (digit(hex[offset]) << 4u) | digit(hex[offset + 1]);
            };

            const auto red = byte(0);
            const auto green = byte(2);
            const auto blue = byte(4);
            const auto alpha = hex.size() == 8 ? byte(6) : 255u;
            return from_rgba8((red << 24u) | (green << 16u) | (blue << 8u) | alpha);
        }

        /**
         * @brief Converts the color to a four-component vector.
         * @return Vector containing red, green, blue and alpha channels.
         */
        [[nodiscard]] constexpr Vec4 to_vec4() const noexcept {
            return {r_, g_, b_, a_};
        }

        /**
         * @brief Creates a color from a four-component vector.
         * @param vec Vector containing red, green, blue and alpha channels.
         * @return Constructed color.
         */
        [[nodiscard]]  static constexpr Color from_vec4(const Vec4& vec) noexcept {
            return {vec.x(), vec.y(), vec.z(), vec.w()};
        }

        /** @return Red channel. */
        [[nodiscard]] constexpr float r() const noexcept { return r_; }
        /** @return Green channel. */
        [[nodiscard]] constexpr float g() const noexcept { return g_; }
        /** @return Blue channel. */
        [[nodiscard]] constexpr float b() const noexcept { return b_; }
        /** @return Alpha channel. */
        [[nodiscard]] constexpr float a() const noexcept { return a_; }

        /** @param value New red channel value. */
        constexpr void set_r(float value) noexcept { r_ = value; }
        /** @param value New green channel value. */
        constexpr void set_g(float value) noexcept { g_ = value; }
        /** @param value New blue channel value. */
        constexpr void set_b(float value) noexcept { b_ = value; }
        /** @param value New alpha channel value. */
        constexpr void set_a(float value) noexcept { a_ = value; }

        /**
         * @brief Returns a copy with a replaced alpha channel.
         * @param alpha New alpha channel.
         * @return Color with unchanged RGB and replaced alpha.
         */
        [[nodiscard]] constexpr Color with_alpha(float alpha) const noexcept {
            return {r_, g_, b_, alpha};
        }

        /**
         * @brief Clamps every channel to the range [0, 1].
         * @return Clamped color.
         */
        [[nodiscard]] Color clamped() const noexcept {
            return {
                std::clamp(r_, 0.f, 1.f),
                std::clamp(g_, 0.f, 1.f),
                std::clamp(b_, 0.f, 1.f),
                std::clamp(a_, 0.f, 1.f)
            };
        }

        /**
         * @brief Multiplies all channels by a scalar.
         * @param scalar Multiplication factor.
         * @return Scaled color.
         */
        [[nodiscard]] constexpr Color operator*(float scalar) const noexcept {
            return {r_ * scalar, g_ * scalar, b_ * scalar, a_ * scalar};
        }

        /**
         * @brief Multiplies color channels component-wise.
         * @param other Right-hand color.
         * @return Component-wise product.
         */
        [[nodiscard]] constexpr Color operator*(const Color& other) const noexcept {
            return {r_ * other.r_, g_ * other.g_, b_ * other.b_, a_ * other.a_};
        }
        /**
         * @brief Adds color channels component-wise.
         * @param other Right-hand color.
         * @return Component-wise sum.
         */
        [[nodiscard]] constexpr Color operator+(const Color& other) const noexcept {
            return {r_ + other.r_, g_ + other.g_, b_ + other.b_, a_ + other.a_};
        }

        /**
         * @brief Multiplies RGB channels by a scalar in place.
         * @param scalar Multiplication factor.
         * @return Reference to this color.
         * @note The alpha channel is intentionally unchanged.
         */
        constexpr Color& operator*=(float scalar) noexcept {
            r_ *= scalar;
            g_ *= scalar;
            b_ *= scalar;
            return *this;
        }

        /**
         * @brief Linearly interpolates between two colors.
         * @param first Start color.
         * @param second End color.
         * @param factor Interpolation factor, clamped to [0, 1].
         * @return Interpolated color.
         */
        [[nodiscard]] static Color lerp(const Color& first, const Color& second, float factor) noexcept {
            factor = std::clamp(factor, 0.0f, 1.0f);
            return {
                first.r_ + (second.r_ - first.r_) * factor,
                first.g_ + (second.g_ - first.g_) * factor,
                first.b_ + (second.b_ - first.b_) * factor,
                first.a_ + (second.a_ - first.a_) * factor,
            };
        }

        /** @return Opaque white. */
        [[nodiscard]] static constexpr Color white() noexcept { return {1.0f, 1.0f, 1.0f, 1.0f}; }
        /** @return Opaque black. */
        [[nodiscard]] static constexpr Color black() noexcept { return {0.0f, 0.0f, 0.0f, 1.0f}; }
        /** @return Opaque red. */
        [[nodiscard]] static constexpr Color red() noexcept { return {1.0f, 0.0f, 0.0f, 1.0f}; }
        /** @return Opaque green. */
        [[nodiscard]] static constexpr Color green() noexcept { return {0.0f, 1.0f, 0.0f, 1.0f}; }
        /** @return Opaque blue. */
        [[nodiscard]] static constexpr Color blue() noexcept { return {0.0f, 0.0f, 1.0f, 1.0f}; }
        /** @return Fully transparent black. */
        [[nodiscard]] static constexpr Color transparent() noexcept { return {0.0f, 0.0f, 0.0f, 0.0f}; }

    private:
        float r_{0.0f};
        float g_{0.0f};
        float b_{0.0f};
        float a_{1.0f};
    };

    /**
     * @brief Multiplies all color channels by a scalar.
     * @param scalar Multiplication factor.
     * @param color Color to scale.
     * @return Scaled color.
     */
    [[nodiscard]] constexpr Color operator*(float scalar, const Color& color) noexcept {
        return color * scalar;
    }

    // Backwards-compatible namespace used by the renderer and scene APIs.
    namespace Math {
        using Color = ::Engine::Color;
    }
}
