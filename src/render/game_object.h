#pragma once

#include "mesh.h"
#include "../core/Vec3.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

// Renderable scene entity with local transform and geometry.
class GameObject {
public:
    virtual ~GameObject() = default;

    Mesh mesh;
    Vec3 position{0.0f, 0.0f, 0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f, 1.0f, 1.0f};

    [[nodiscard]] glm::mat4 modelMatrix() const noexcept {
        return glm::translate(glm::mat4{1.0f}, position.native()) *
               glm::mat4_cast(rotation) *
               glm::scale(glm::mat4{1.0f}, scale.native());
    }
};
