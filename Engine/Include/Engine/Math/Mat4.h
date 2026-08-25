#pragma once

#include "Engine/Math/Angle.h"
#include "Engine/Math/Quat.h"
#include "Engine/Math/Vec3.h"
#include "Engine/Math/Vec4.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace Engine {
    class Mat4 {
    public:
        Mat4() = default;

        explicit Mat4(const glm::mat4 &value)
            : m_value(value) {
        }

        /** @brief Returns an identity matrix translated by position. */
        [[nodiscard]] static Mat4 translate(const Vec3 &position) noexcept {
            return Mat4{glm::translate(glm::mat4{1.0F}, position.native())};
        }

        /**
         * @brief Returns a right-handed view matrix looking from @p eye towards @p center.
         * @param eye Camera position in world space.
         * @param center Point the camera looks at.
         * @param up World-space up direction.
         */
        [[nodiscard]] static Mat4 lookAt(const Vec3 &eye, const Vec3 &center, const Vec3 &up) noexcept {
            return Mat4{glm::lookAt(eye.native(), center.native(), up.native())};
        }

        /**
         * @brief Returns a right-handed orthographic projection matrix.
         *
         * The depth range follows the GLM configuration in effect when this header
         * is included (use @c GLM_FORCE_DEPTH_ZERO_TO_ONE for Vulkan).
         */
        [[nodiscard]] static Mat4 ortho(const float left, const float right,
                                        const float bottom, const float top,
                                        const float nearPlane, const float farPlane) noexcept {
            return Mat4{glm::ortho(left, right, bottom, top, nearPlane, farPlane)};
        }

        /**
         * @brief Returns a perspective projection matrix.
         *
         * The depth range follows the GLM configuration in effect when this header
         * is included (use @c GLM_FORCE_DEPTH_ZERO_TO_ONE for Vulkan).
         */
        [[nodiscard]] static Mat4 perspective(const Radians verticalFov,
                                              const float aspectRatio,
                                              const float nearPlane,
                                              const float farPlane) noexcept {
            return Mat4{
                glm::perspective(
                    verticalFov.value(), aspectRatio, nearPlane, farPlane)
            };
        }

        /** @brief Returns matrix rotated by angle radians around axis. */
        [[nodiscard]] static Mat4 rotate(const Mat4 &matrix, const Radians angle, const Vec3 &axis) noexcept {
            return Mat4{glm::rotate(matrix.m_value, angle.value(), axis.native())};
        }

        /** @brief Returns a matrix representing the given quaternion rotation. */
        [[nodiscard]] static Mat4 rotate(const Quat &rotation) noexcept {
            return Mat4{glm::mat4_cast(rotation.native())};
        }

        /** @brief Returns matrix scaled by the given per-axis factors. */
        [[nodiscard]] static Mat4 scale(const Mat4 &matrix, const Vec3 &factors) noexcept {
            return Mat4{glm::scale(matrix.m_value, factors.native())};
        }

        [[nodiscard]] glm::mat4 &native() noexcept {
            return m_value;
        }

        [[nodiscard]] const glm::mat4 &native() const noexcept {
            return m_value;
        }

        /** @brief Returns the composition of this transform followed by rhs. */
        [[nodiscard]] Mat4 operator*(const Mat4 &rhs) const noexcept {
            return Mat4{m_value * rhs.m_value};
        }

        /** @brief Returns this matrix applied to a homogeneous vector. */
        [[nodiscard]] Vec4 operator*(const Vec4 &rhs) const noexcept {
            return Vec4{m_value * rhs.native()};
        }

    private:
        glm::mat4 m_value{1.0F};
    };
} // namespace Engine