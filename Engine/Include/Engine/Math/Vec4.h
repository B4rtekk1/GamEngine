#pragma once

/** @file Vec4.h Four-dimensional vector type. */

#include <glm/glm.hpp>

namespace Engine {

/**
 * @brief Four-dimensional floating-point vector used by the engine.
 *
 * Vec4 is suitable for homogeneous coordinates, shader data and RGBA values.
 */
class Vec4 {
public:
    /** @brief Constructs a zero vector. */
    constexpr Vec4() = default;
    /** @brief Constructs a vector from its four components. */
    constexpr Vec4(float x, float y, float z, float w) : m_value(x, y, z, w) {}
    /** @brief Constructs a vector from its native GLM representation. */
    explicit constexpr Vec4(const glm::vec4& value) : m_value(value) {}

    /** @brief Returns the X component. */
    [[nodiscard]] constexpr float x() const noexcept { return m_value.x; }
    /** @brief Returns the Y component. */
    [[nodiscard]] constexpr float y() const noexcept { return m_value.y; }
    /** @brief Returns the Z component. */
    [[nodiscard]] constexpr float z() const noexcept { return m_value.z; }
    /** @brief Returns the W component. */
    [[nodiscard]] constexpr float w() const noexcept { return m_value.w; }
    /** @brief Sets the X component. */
    constexpr void setX(float value) noexcept { m_value.x = value; }
    /** @brief Sets the Y component. */
    constexpr void setY(float value) noexcept { m_value.y = value; }
    /** @brief Sets the Z component. */
    constexpr void setZ(float value) noexcept { m_value.z = value; }
    /** @brief Sets the W component. */
    constexpr void setW(float value) noexcept { m_value.w = value; }

    /** @brief Returns the component-wise sum. */
    constexpr Vec4 operator+(const Vec4& rhs) const noexcept { return Vec4{m_value + rhs.m_value}; }
    /** @brief Returns the component-wise difference. */
    constexpr Vec4 operator-(const Vec4& rhs) const noexcept { return Vec4{m_value - rhs.m_value}; }
    /** @brief Returns the negated vector. */
    constexpr Vec4 operator-() const noexcept { return Vec4{-m_value}; }
    /** @brief Returns the component-wise product. */
    constexpr Vec4 operator*(const Vec4& rhs) const noexcept { return Vec4{m_value * rhs.m_value}; }
    /** @brief Returns the vector multiplied by a scalar. */
    constexpr Vec4 operator*(float scalar) const noexcept { return Vec4{m_value * scalar}; }
    /** @brief Adds another vector component-wise. */
    constexpr Vec4& operator+=(const Vec4& rhs) noexcept { return *this = *this + rhs; }
    /** @brief Subtracts another vector component-wise. */
    constexpr Vec4& operator-=(const Vec4& rhs) noexcept { return *this = *this - rhs; }
    /** @brief Multiplies this vector component-wise. */
    constexpr Vec4& operator*=(const Vec4& rhs) noexcept { return *this = *this * rhs; }
    /** @brief Multiplies this vector by a scalar. */
    constexpr Vec4& operator*=(float scalar) noexcept { return *this = *this * scalar; }

    /** @brief Returns the Euclidean length. */
    [[nodiscard]] float length() const noexcept { return glm::length(m_value); }
    /** @brief Returns a normalized copy of this vector. */
    [[nodiscard]] Vec4 normalized() const noexcept { return Vec4{glm::normalize(m_value)}; }
    /** @brief Returns the native GLM representation. */
    [[nodiscard]] constexpr glm::vec4 native() const noexcept { return m_value; }

private:
    glm::vec4 m_value{};
};

/** @brief Returns a vector multiplied by a scalar. */
constexpr Vec4 operator*(float scalar, const Vec4& value) noexcept { return value * scalar; }

} // namespace Engine
