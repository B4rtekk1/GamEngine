#pragma once

/** @file quat.h Quaternion rotation type. */

#include "Engine/Math/vec3.h"

#include <glm/gtc/quaternion.hpp>

namespace Engine {

/**
 * @brief Unit quaternion used to represent three-dimensional rotations.
 *
 * The default value is the identity rotation. Quaternion multiplication
 * composes rotations; `a * b` applies b first and then a.
 */
class quat {
public:
    /** @brief Constructs the identity rotation. */
    constexpr quat() : m_value(1.0f, 0.0f, 0.0f, 0.0f) {}

    /** @brief Constructs a quaternion from its scalar and vector components. */
    constexpr quat(float w, float x, float y, float z) : m_value(w, x, y, z) {}

    /** @brief Constructs a quaternion from GLM's native representation. */
    explicit constexpr quat(const glm::quat& value) : m_value(value) {}

    /** @brief Creates a rotation by angleRadians about axis. */
    [[nodiscard]] static quat angleAxis(float angleRadians, const vec3& axis) noexcept {
        return quat{glm::angleAxis(angleRadians, glm::normalize(axis.native()))};
    }

    /** @brief Returns the scalar component. */
    [[nodiscard]] constexpr float w() const noexcept { return m_value.w; }
    /** @brief Returns the X vector component. */
    [[nodiscard]] constexpr float x() const noexcept { return m_value.x; }
    /** @brief Returns the Y vector component. */
    [[nodiscard]] constexpr float y() const noexcept { return m_value.y; }
    /** @brief Returns the Z vector component. */
    [[nodiscard]] constexpr float z() const noexcept { return m_value.z; }

    /** @brief Returns the composition of this rotation with rhs. */
    [[nodiscard]] constexpr quat operator*(const quat& rhs) const noexcept {
        return quat{m_value * rhs.m_value};
    }

    /** @brief Composes this rotation with rhs. */
    constexpr quat& operator*=(const quat& rhs) noexcept { return *this = *this * rhs; }

    /** @brief Rotates a vector by this quaternion. */
    [[nodiscard]] vec3 operator*(const vec3& value) const noexcept {
        return vec3{m_value * value.native()};
    }

    /** @brief Returns a normalized copy of this quaternion. */
    [[nodiscard]] quat normalized() const noexcept { return quat{glm::normalize(m_value)}; }

    /** @brief Returns the native GLM representation. */
    [[nodiscard]] constexpr glm::quat native() const noexcept { return m_value; }

private:
    glm::quat m_value;
};

} // namespace Engine
