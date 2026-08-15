#pragma once

/** @file Vec2.h Two-dimensional vector type. */

#include <glm/glm.hpp>

namespace Engine {
    /**
     * @brief Two-dimensional floating-point vector used by the engine.
     */
    class Vec2 {
    public:
        /** @brief Constructs a zero vector. */
        constexpr Vec2() = default;

        /** @brief Constructs a vector from its two components. */
        constexpr Vec2(float x, float y) : m_value(x, y) {
        }

        /** @brief Constructs a vector from its native GLM representation. */
        explicit constexpr Vec2(const glm::vec2 &value) : m_value(value) {
        }

        /** @brief Returns the X component. */
        [[nodiscard]] constexpr float x() const noexcept { return m_value.x; }
        /** @brief Returns the Y component. */
        [[nodiscard]] constexpr float y() const noexcept { return m_value.y; }
        /** @brief Sets the X component. */
        constexpr void setX(const float value) noexcept { m_value.x = value; }
        /** @brief Sets the Y component. */
        constexpr void setY(const float value) noexcept { m_value.y = value; }

        /** @brief Returns the component-wise sum. */
        constexpr Vec2 operator+(const Vec2 &rhs) const noexcept { return Vec2{m_value + rhs.m_value}; }
        /** @brief Returns the component-wise difference. */
        constexpr Vec2 operator-(const Vec2 &rhs) const noexcept { return Vec2{m_value - rhs.m_value}; }
        /** @brief Returns the negated vector. */
        constexpr Vec2 operator-() const noexcept { return Vec2{-m_value}; }
        /** @brief Returns the component-wise product. */
        constexpr Vec2 operator*(const Vec2 &rhs) const noexcept { return Vec2{m_value * rhs.m_value}; }
        /** @brief Returns the vector multiplied by a scalar. */
        constexpr Vec2 operator*(float scalar) const noexcept { return Vec2{m_value * scalar}; }
        /** @brief Adds another vector component-wise. */
        constexpr Vec2 &operator+=(const Vec2 &rhs) noexcept { return *this = *this + rhs; }
        /** @brief Subtracts another vector component-wise. */
        constexpr Vec2 &operator-=(const Vec2 &rhs) noexcept { return *this = *this - rhs; }
        /** @brief Multiplies this vector component-wise. */
        constexpr Vec2 &operator*=(const Vec2 &rhs) noexcept { return *this = *this * rhs; }
        /** @brief Multiplies this vector by a scalar. */
        constexpr Vec2 &operator*=(float scalar) noexcept { return *this = *this * scalar; }

        /** @brief Returns the Euclidean length. */
        [[nodiscard]] float length() const noexcept { return glm::length(m_value); }
        /** @brief Returns a normalized copy of this vector. */
        [[nodiscard]] Vec2 normalized() const noexcept { return Vec2{glm::normalize(m_value)}; }
        /** @brief Returns the native GLM representation. */
        [[nodiscard]] constexpr glm::vec2 native() const noexcept { return m_value; }

    private:
        glm::vec2 m_value{};
    };

    /** @brief Returns a vector multiplied by a scalar. */
    constexpr Vec2 operator*(float scalar, const Vec2 &value) noexcept { return value * scalar; }
} // namespace Engine
