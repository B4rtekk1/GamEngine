/**
 * @file Trigonometry.h
 * @brief Trigonometric utilities used throughout the graphics engine.
 */

#pragma once

#include "Angle.h"

#include <cmath>

namespace Engine {

    /** @brief Returns the sine of a value interpreted as radians. */
    [[nodiscard]] inline float sin(const float angleRadians) noexcept {
        return std::sin(angleRadians);
    }

    /** @brief Returns the sine of an angle expressed in radians. */
    [[nodiscard]] inline float sin(const Radians angle) noexcept {
        return sin(angle.value());
    }

    /** @brief Returns the sine of an angle expressed in degrees. */
    [[nodiscard]] inline float sin(const Degrees angle) noexcept {
        return sin(angle.toRadians());
    }

    /** @brief Returns the cosine of a value interpreted as radians. */
    [[nodiscard]] inline float cos(const float angleRadians) noexcept {
        return std::cos(angleRadians);
    }

    /** @brief Returns the cosine of an angle expressed in radians. */
    [[nodiscard]] inline float cos(const Radians angle) noexcept {
        return cos(angle.value());
    }

    /** @brief Returns the cosine of an angle expressed in degrees. */
    [[nodiscard]] inline float cos(const Degrees angle) noexcept {
        return cos(angle.toRadians());
    }

    /** @brief Returns the tangent of an angle expressed in radians. */
    [[nodiscard]] inline float tan(const float angleRadians) noexcept {
        return std::tan(angleRadians);
    }

    /** @brief Returns the tangent of an angle expressed in radians. */
    [[nodiscard]] inline float tan(const Radians angle) noexcept {
        return tan(angle.value());
    }

    /** @brief Returns the tangent of an angle expressed in degrees. */
    [[nodiscard]] inline float tan(const Degrees angle) noexcept {
        return tan(angle.toRadians());
    }

    /** @brief Returns the arcsine of @p value as a radians angle. */
    [[nodiscard]] inline Radians asin(const float value) noexcept {
        return Radians{std::asin(value)};
    }

    /** @brief Returns the arccosine of @p value as a radians angle. */
    [[nodiscard]] inline Radians acos(const float value) noexcept {
        return Radians{std::acos(value)};
    }

    /** @brief Returns the arctangent of @p value as a radians angle. */
    [[nodiscard]] inline Radians atan(const float value) noexcept {
        return Radians{std::atan(value)};
    }

    /** @brief Returns the arctangent of @p y / @p x as a radians angle. */
    [[nodiscard]] inline Radians atan2(const float y, const float x) noexcept {
        return Radians{std::atan2(y, x)};
    }

    /** @brief Returns the cotangent of an angle expressed in radians. */
    [[nodiscard]] inline float cot(const Radians angle) noexcept {
        return 1.0F / tan(angle);
    }

    /** @brief Returns the cotangent of an angle expressed in degrees. */
    [[nodiscard]] inline float cot(const Degrees angle) noexcept {
        return cot(angle.toRadians());
    }

    /** @brief Returns the secant of an angle expressed in radians. */
    [[nodiscard]] inline float sec(const Radians angle) noexcept {
        return 1.0F / cos(angle);
    }

    /** @brief Returns the secant of an angle expressed in degrees. */
    [[nodiscard]] inline float sec(const Degrees angle) noexcept {
        return sec(angle.toRadians());
    }

    /** @brief Returns the cosecant of an angle expressed in radians. */
    [[nodiscard]] inline float csc(const Radians angle) noexcept {
        return 1.0F / sin(angle);
    }

    /** @brief Returns the cosecant of an angle expressed in degrees. */
    [[nodiscard]] inline float csc(const Degrees angle) noexcept {
        return csc(angle.toRadians());
    }
}
