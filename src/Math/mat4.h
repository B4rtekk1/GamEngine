#pragma once

#include "../core/math/translate.h"

#include <glm/glm.hpp>

class mat4 {
public:
    mat4() = default;

    explicit mat4(const glm::mat4& value)
        : m_value(value) {}

    [[nodiscard]] static mat4 translate(const vec3& position) {
        return mat4{::translate(position)};
    }

    [[nodiscard]] const glm::mat4& native() const noexcept {
        return m_value;
    }

private:
    glm::mat4 m_value{1.0f};
};
