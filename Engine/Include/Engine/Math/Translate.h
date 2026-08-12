#pragma once

#include "Engine/Math/Vec3.h"

#include <glm/glm.hpp>

namespace Engine {

/** @brief Returns an identity matrix translated by the given position. */
[[nodiscard]] inline glm::mat4 translate(const Vec3& position) noexcept {
    return glm::mat4{
        glm::vec4{1.0f, 0.0f, 0.0f, 0.0f},
        glm::vec4{0.0f, 1.0f, 0.0f, 0.0f},
        glm::vec4{0.0f, 0.0f, 1.0f, 0.0f},
        glm::vec4{position.x(), position.y(), position.z(), 1.0f},
    };
}

} // namespace Engine
