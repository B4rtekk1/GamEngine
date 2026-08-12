#pragma once

#include "Engine/Math/translate.h"

#include <glm/glm.hpp>

namespace Engine {

class Mat4 {
public:
    Mat4() = default;

    explicit Mat4(const glm::mat4& value)
        : m_value(value) {}

    [[nodiscard]] static Mat4 translate(const vec3& position) {
        return Mat4{::translate(position)};
    }

    [[nodiscard]] const glm::mat4& native() const noexcept {
        return m_value;
    }

private:
    glm::mat4 m_value{1.0f};
};

} // namespace Engine
