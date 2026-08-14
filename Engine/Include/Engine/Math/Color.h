#pragma once

#include "Engine/Math/Vec4.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace Engine::Math {
    class Color final {
    public:
        constexpr Color() noexcept = default;

        constexpr Color(float red, float green, float blue, float alpha = 1.0) noexcept : r_(red), g_(green), b_(blue), a_(alpha) {}

        [[nodiscard]] static constexpr Color from_rgb(float red, float green, float blue) noexcept {
            return {red, green, blue, 1.0f};
        }

        [[nodiscard]] static constexpr Color from_rgba(float red, float green, float blue, float alpha) noexcept {
            return {red, green, blue, alpha};
        }

        [[nodiscard]] static constexpr Color from_rgba8(std::uint32_t value) noexcept {
            return {
                static_cast<float>((value >> 24) & 0xffu) / 255.0f,
                static_cast<float>((value >> 16) & 0xffu) / 255.0f,
                static_cast<float>((value >> 8) & 0xffu) / 255.0f,
                static_cast<float>((value) & 0xffu) / 255.0f,
            };
        }

        // Vulkan's VK_FORMAT_A2B10G10R10_UNORM_PACK32 layout:
        // R occupies bits 0..9, G 10..19, B 20..29 and A 30..31.
        [[nodiscard]] static constexpr Color from_a2b10g10r10(std::uint32_t value) noexcept {
            return {
                static_cast<float>(value & 0x3ffu) / 1023.0f,
                static_cast<float>((value >> 10u) & 0x3ffu) / 1023.0f,
                static_cast<float>((value >> 20u) & 0x3ffu) / 1023.0f,
                static_cast<float>((value >> 30u) & 0x3u) / 3.0f,
            };
        }

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

        [[nodiscard]] constexpr Vec4 to_vec4() const noexcept {
            return Vec4(r_, g_, b_, a_);
        }

        [[nodiscard]]  static constexpr Color from_vec4(const Vec4& vec) noexcept {
            return {vec.x(), vec.y(), vec.z(), vec.w()};
        }

        [[nodiscard]] constexpr float r() const noexcept { return r_; }
        [[nodiscard]] constexpr float g() const noexcept { return g_; }
        [[nodiscard]] constexpr float b() const noexcept { return b_; }
        [[nodiscard]] constexpr float a() const noexcept { return a_; }

        constexpr void set_r(float value) noexcept { r_ = value; }
        constexpr void set_g(float value) noexcept { g_ = value; }
        constexpr void set_b(float value) noexcept { b_ = value; }
        constexpr void set_a(float value) noexcept { a_ = value; }

        [[nodiscard]] constexpr Color with_alpha(float alpha) const noexcept {
            return {r_, g_, b_, alpha};
        }

        [[nodiscard]] Color clamped() const noexcept {
            return {
                std::clamp(r_, 0.f, 1.f),
                std::clamp(g_, 0.f, 1.f),
                std::clamp(b_, 0.f, 1.f),
                std::clamp(a_, 0.f, 1.f)
            };
        }

        [[nodiscard]] constexpr Color operator*(float scalar) const noexcept {
            return {r_ * scalar, g_ * scalar, b_ * scalar, a_ * scalar};
        }

        [[nodiscard]] constexpr Color operator*(const Color& other) const noexcept {
            return {r_ * other.r_, g_ * other.g_, b_ * other.b_, a_ * other.a_};
        }
        [[nodiscard]] constexpr Color operator+(const Color& other) const noexcept {
            return {r_ + other.r_, g_ + other.g_, b_ + other.b_, a_ + other.a_};
        }

        constexpr Color& operator*=(float scalar) noexcept {
            r_ *= scalar;
            g_ *= scalar;
            b_ *= scalar;
            return *this;
        }

        [[nodiscard]] static Color lerp(const Color& first, const Color& second, float factor) noexcept {
            factor = std::clamp(factor, 0.0f, 1.0f);
            return {
                first.r_ + (second.r_ - first.r_) * factor,
                first.g_ + (second.g_ - first.g_) * factor,
                first.b_ + (second.b_ - first.b_) * factor,
                first.a_ + (second.a_ - first.a_) * factor,
            };
        }

        [[nodiscard]] static constexpr Color white() noexcept { return {1.0f, 1.0f, 1.0f, 1.0f}; }
        [[nodiscard]] static constexpr Color black() noexcept { return {0.0f, 0.0f, 0.0f, 1.0f}; }
        [[nodiscard]] static constexpr Color red() noexcept { return {1.0f, 0.0f, 0.0f, 1.0f}; }
        [[nodiscard]] static constexpr Color green() noexcept { return {0.0f, 1.0f, 0.0f, 1.0f}; }
        [[nodiscard]] static constexpr Color blue() noexcept { return {0.0f, 0.0f, 1.0f, 1.0f}; }
        [[nodiscard]] static constexpr Color transparent() noexcept { return {0.0f, 0.0f, 0.0f, 0.0f}; }

    private:
        float r_{0.0f};
        float g_{0.0f};
        float b_{0.0f};
        float a_{1.0f};
    };

    [[nodiscard]] constexpr Color operator*(float scalar, const Color& color) noexcept {
        return color * scalar;
    }
}
