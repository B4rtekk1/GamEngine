#pragma once

/** @file quat.h Quaternion rotation type. */

#include "Engine/Math/Vec3.h"

#include <glm/gtc/quaternion.hpp>

namespace Engine {
    /**
     * @brief Unit quaternion used to represent three-dimensional rotations.
     *
     * The default value is the identity rotation. Quaternion multiplication
     * composes rotations; `a * b` applies b first and then a.
     */
    class Quat {
    public:
        /** @brief Constructs the identity rotation. */
        constexpr Quat() : m_value(1.0F, 0.0F, 0.0F, 0.0F) {
        }

        /** @brief Constructs a quaternion from its scalar and vector components. */
        constexpr Quat(float w, float x, float y, float z) : m_value(w, x, y, z) {
        }

        /** @brief Constructs a quaternion from GLM's native representation. */
        explicit constexpr Quat(const glm::quat &value) : m_value(value) {
        }

        /** @brief Creates a rotation by angleRadians about axis. */
        [[nodiscard]] static Quat angleAxis(float angleRadians, const Vec3 &axis) noexcept {
            return Quat{glm::angleAxis(angleRadians, glm::normalize(axis.native()))};
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
        [[nodiscard]] constexpr Quat operator*(const Quat &rhs) const noexcept {
            return Quat{m_value * rhs.m_value};
        }

        /** @brief Composes this rotation with rhs. */
        constexpr Quat &operator*=(const Quat &rhs) noexcept { return *this = *this * rhs; }

        /** @brief Rotates a vector by this quaternion. */
        [[nodiscard]] Vec3 operator*(const Vec3 &value) const noexcept {
            return Vec3{m_value * value.native()};
        }

        /** @brief Returns a normalized copy of this quaternion. */
        [[nodiscard]] Quat normalized() const noexcept { return Quat{glm::normalize(m_value)}; }

        /** @brief Returns the native GLM representation. */
        [[nodiscard]] constexpr glm::quat native() const noexcept { return m_value; }

    private:
        glm::quat m_value;
    };
} // namespace Engine