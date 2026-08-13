#pragma once

/** @file transform.h Object transform value type. */

#include "../Math/Vec3.h"

#include <glm/gtc/matrix_transform.hpp>

namespace Engine {

/** @brief Position, Euler rotation and scale of a renderable object. */
struct Transform {
    /** @brief World-space position. */
    Vec3 position{};
    /** @brief Euler rotation, in the engine's rotation units. */
    Vec3 rotation{};
    /** @brief Per-axis scale; defaults to one on every axis. */
    Vec3 scale{1.0f, 1.0f, 1.0f};

    /** @brief Builds the local-to-world matrix for rendering. */
    [[nodiscard]] glm::mat4 matrix() const noexcept {
        glm::mat4 result = glm::translate(glm::mat4{1.0f}, position.native());
        result = glm::rotate(result, glm::radians(rotation.x()), Vec3{1.0f, 0.0f, 0.0f}.native());
        result = glm::rotate(result, glm::radians(rotation.y()), Vec3{0.0f, 1.0f, 0.0f}.native());
        result = glm::rotate(result, glm::radians(rotation.z()), Vec3{0.0f, 0.0f, 1.0f}.native());
        return glm::scale(result, scale.native());
    }
};

} // namespace Engine
