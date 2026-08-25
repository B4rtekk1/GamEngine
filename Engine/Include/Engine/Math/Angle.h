#pragma once

/** @file Angle.h Strongly typed angle units and explicit conversions. */

namespace Engine {
    class Degrees;

    /** @brief An angle stored in radians. */
    class Radians {
    public:
        constexpr Radians() = default;

        explicit constexpr Radians(float value) noexcept : m_value(value) {
        }

        explicit constexpr Radians(Degrees value) noexcept;

        [[nodiscard]] constexpr float value() const noexcept { return m_value; }

        [[nodiscard]] constexpr Degrees toDegrees() const noexcept;

        [[nodiscard]] constexpr Radians operator-() const noexcept { return Radians{-m_value}; }
        [[nodiscard]] constexpr Radians operator+(Radians rhs) const noexcept { return Radians{m_value + rhs.m_value}; }
        [[nodiscard]] constexpr Radians operator-(Radians rhs) const noexcept { return Radians{m_value - rhs.m_value}; }
        [[nodiscard]] constexpr Radians operator*(float scalar) const noexcept { return Radians{m_value * scalar}; }
        [[nodiscard]] constexpr Radians operator/(float scalar) const noexcept { return Radians{m_value / scalar}; }
        constexpr Radians &operator+=(Radians rhs) noexcept { return *this = *this + rhs; }
        constexpr Radians &operator-=(Radians rhs) noexcept { return *this = *this - rhs; }
        constexpr Radians &operator*=(float scalar) noexcept { return *this = *this * scalar; }
        constexpr Radians &operator/=(float scalar) noexcept { return *this = *this / scalar; }

        [[nodiscard]] constexpr bool operator==(const Radians &rhs) const noexcept = default;

    private:
        float m_value{};
    };

    /** @brief An angle stored in degrees. */
    class Degrees {
    public:
        constexpr Degrees() = default;

        explicit constexpr Degrees(float value) noexcept : m_value(value) {
        }

        explicit constexpr Degrees(Radians value) noexcept;

        [[nodiscard]] constexpr float value() const noexcept { return m_value; }

        [[nodiscard]] constexpr Radians toRadians() const noexcept;

        [[nodiscard]] constexpr Degrees operator-() const noexcept { return Degrees{-m_value}; }
        [[nodiscard]] constexpr Degrees operator+(Degrees rhs) const noexcept { return Degrees{m_value + rhs.m_value}; }
        [[nodiscard]] constexpr Degrees operator-(Degrees rhs) const noexcept { return Degrees{m_value - rhs.m_value}; }
        [[nodiscard]] constexpr Degrees operator*(float scalar) const noexcept { return Degrees{m_value * scalar}; }
        [[nodiscard]] constexpr Degrees operator/(float scalar) const noexcept { return Degrees{m_value / scalar}; }
        constexpr Degrees &operator+=(Degrees rhs) noexcept { return *this = *this + rhs; }
        constexpr Degrees &operator-=(Degrees rhs) noexcept { return *this = *this - rhs; }
        constexpr Degrees &operator*=(float scalar) noexcept { return *this = *this * scalar; }
        constexpr Degrees &operator/=(float scalar) noexcept { return *this = *this / scalar; }

        [[nodiscard]] constexpr bool operator==(const Degrees &rhs) const noexcept = default;

    private:
        float m_value{};
    };

    inline constexpr float kRadiansPerDegree = 0.01745329251994329577F;
    inline constexpr float kDegreesPerRadian = 57.29577951308232088F;

    constexpr Radians::Radians(const Degrees value) noexcept : m_value(value.value() * kRadiansPerDegree) {
    }

    constexpr Degrees Radians::toDegrees() const noexcept { return Degrees{m_value * kDegreesPerRadian}; }

    constexpr Degrees::Degrees(const Radians value) noexcept : m_value(value.value() * kDegreesPerRadian) {
    }

    constexpr Radians Degrees::toRadians() const noexcept { return Radians{m_value * kRadiansPerDegree}; }

    [[nodiscard]] constexpr Radians operator*(float scalar, Radians value) noexcept { return value * scalar; }
    [[nodiscard]] constexpr Degrees operator*(float scalar, Degrees value) noexcept { return value * scalar; }
} // namespace Engine