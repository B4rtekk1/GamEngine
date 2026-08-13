#pragma once

#include "Engine/Math/Radians.h"
#include "Engine/Math/Vec3.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace Engine {

class Mat4 {
public:
    Mat4() = default;

    explicit Mat4(const glm::mat4& value)
        : m_value(value) {}

    /** @brief Returns an identity matrix translated by position. */
    [[nodiscard]] static Mat4 translate(const Vec3& position) noexcept {
        return Mat4{glm::translate(glm::mat4{1.0f}, position.native())};
    }

    /** @brief Returns matrix rotated by angle radians around axis. */
    [[nodiscard]] static Mat4 rotate(const Mat4& matrix, const Radians angle, const Vec3& axis) noexcept {
        return Mat4{glm::rotate(matrix.m_value, angle.value(), axis.native())};
    }

    /** @brief Returns matrix scaled by the given per-axis factors. */
    [[nodiscard]] static Mat4 scale(const Mat4& matrix, const Vec3& factors) noexcept {
        return Mat4{glm::scale(matrix.m_value, factors.native())};
    }

    [[nodiscard]] const glm::mat4& native() const noexcept {
        return m_value;
    }

private:
    glm::mat4 m_value{1.0f};
};

} // namespace Engine
