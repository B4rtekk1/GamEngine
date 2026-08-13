#pragma once

/** @file Radians.h Strongly typed angle expressed in radians. */

namespace Engine {

/**
 * @brief An angle stored in radians.
 *
 * Use fromDegrees() when the input is expressed in degrees. Conversion back to
 * a raw floating-point value is explicit to avoid mixing angles and scalars
 * accidentally.
 */
class Radians {
public:
    /** @brief Constructs a zero Radian. */
    constexpr Radians() = default;

    /** @brief Constructs an angle from a value expressed in radians. */
    explicit constexpr Radians(float angle) noexcept : m_value(angle) {}

    /** @brief Creates an angle from a value expressed in degrees. */
    [[nodiscard]] static constexpr Radians fromDegrees(float angle) noexcept {
        return Radians{angle * kRadiansPerDegree};
    }

    /** @brief Returns the angle in radians. */
    [[nodiscard]] constexpr float value() const noexcept { return m_value; }

    /** @brief Returns the angle converted to degrees. */
    [[nodiscard]] constexpr float degrees() const noexcept { return m_value * kDegreesPerRadian; }

    /** @brief Returns the negated angle. */
    [[nodiscard]] constexpr Radians operator-() const noexcept { return Radians{-m_value}; }
    /** @brief Adds two angles. */
    [[nodiscard]] constexpr Radians operator+(Radians rhs) const noexcept { return Radians{m_value + rhs.m_value}; }
    /** @brief Subtracts rhs from this angle. */
    [[nodiscard]] constexpr Radians operator-(Radians rhs) const noexcept { return Radians{m_value - rhs.m_value}; }
    /** @brief Multiplies this angle by a scalar. */
    [[nodiscard]] constexpr Radians operator*(float scalar) const noexcept { return Radians{m_value * scalar}; }
    /** @brief Divides this angle by a scalar. */
    [[nodiscard]] constexpr Radians operator/(float scalar) const noexcept { return Radians{m_value / scalar}; }

    /** @brief Adds rhs to this angle. */
    constexpr Radians& operator+=(Radians rhs) noexcept { return *this = *this + rhs; }
    /** @brief Subtracts rhs from this angle. */
    constexpr Radians& operator-=(Radians rhs) noexcept { return *this = *this - rhs; }
    /** @brief Multiplies this angle by a scalar. */
    constexpr Radians& operator*=(float scalar) noexcept { return *this = *this * scalar; }
    /** @brief Divides this angle by a scalar. */
    constexpr Radians& operator/=(float scalar) noexcept { return *this = *this / scalar; }

    /** @brief Compares the underlying values exactly. */
    [[nodiscard]] constexpr bool operator==(const Radians& rhs) const noexcept = default;

private:
    static constexpr float kRadiansPerDegree = 0.01745329251994329577f;
    static constexpr float kDegreesPerRadian = 57.29577951308232088f;

    float m_value{};
};

/** @brief Returns an angle multiplied by a scalar. */
[[nodiscard]] constexpr Radians operator*(float scalar, Radians value) noexcept {
    return value * scalar;
}

} // namespace Engine
