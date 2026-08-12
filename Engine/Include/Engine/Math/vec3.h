#pragma once

/** @file Vec3.h Three-dimensional vector type. */

#include <glm/glm.hpp>

namespace Engine {

/**
 * @brief Three-dimensional floating-point vector used by the engine.
 *
 * vec3 keeps the engine-facing API independent from GLM while allowing
 * explicit conversion at library and rendering boundaries.
 */
class vec3 {
public:
    /** @brief Constructs a zero vector. */
    constexpr vec3() = default;

    /** @brief Constructs a vector from its three components. */
    constexpr vec3(float x, float y, float z) : m_value(x, y, z) {}

    /** @brief Constructs a vector from its native GLM representation. */
    explicit constexpr vec3(const glm::vec3& value) : m_value(value) {}

    /** @brief Returns the X component. */
    [[nodiscard]] constexpr float x() const noexcept { return m_value.x; }
    /** @brief Returns the Y component. */
    [[nodiscard]] constexpr float y() const noexcept { return m_value.y; }
    /** @brief Returns the Z component. */
    [[nodiscard]] constexpr float z() const noexcept { return m_value.z; }

    /** @brief Sets the X component. */
    constexpr void setX(float value) noexcept { m_value.x = value; }
    /** @brief Sets the Y component. */
    constexpr void setY(float value) noexcept { m_value.y = value; }
    /** @brief Sets the Z component. */
    constexpr void setZ(float value) noexcept { m_value.z = value; }

    /** @brief Returns the component-wise sum. */
    constexpr vec3 operator+(const vec3& rhs) const noexcept { return vec3{m_value + rhs.m_value}; }
    /** @brief Returns the component-wise difference. */
    constexpr vec3 operator-(const vec3& rhs) const noexcept { return vec3{m_value - rhs.m_value}; }
    /** @brief Returns the negated vector. */
    constexpr vec3 operator-() const noexcept { return vec3{-m_value}; }
    /** @brief Returns the component-wise product. */
    constexpr vec3 operator*(const vec3& rhs) const noexcept { return vec3{m_value * rhs.m_value}; }
    /** @brief Returns the vector multiplied by a scalar. */
    constexpr vec3 operator*(float scalar) const noexcept { return vec3{m_value * scalar}; }
    /** @brief Adds another vector component-wise. */
    constexpr vec3& operator+=(const vec3& rhs) noexcept { return *this = *this + rhs; }
    /** @brief Subtracts another vector component-wise. */
    constexpr vec3& operator-=(const vec3& rhs) noexcept { return *this = *this - rhs; }
    /** @brief Multiplies this vector component-wise. */
    constexpr vec3& operator*=(const vec3& rhs) noexcept { return *this = *this * rhs; }
    /** @brief Multiplies this vector by a scalar. */
    constexpr vec3& operator*=(float scalar) noexcept { return *this = *this * scalar; }

    /** @brief Returns the Euclidean length. */
    [[nodiscard]] float length() const noexcept { return glm::length(m_value); }
    /** @brief Returns a normalized copy of this vector. */
    [[nodiscard]] vec3 normalized() const noexcept { return vec3{glm::normalize(m_value)}; }
    /** @brief Returns the native GLM representation. */
    [[nodiscard]] constexpr glm::vec3 native() const noexcept { return m_value; }

private:
    glm::vec3 m_value{};
};

/** @brief Returns a vector multiplied by a scalar. */
constexpr vec3 operator*(float scalar, const vec3& value) noexcept { return value * scalar; }

} // namespace Engine
