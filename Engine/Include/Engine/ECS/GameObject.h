#pragma once

#include "Engine/Renderer/mesh.h"
#include "Engine/Math/vec3.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Engine {

// Renderable scene entity with local transform and geometry.
class GameObject {
public:
    virtual ~GameObject() = default;

    Mesh mesh;
    vec3 position{0.0f, 0.0f, 0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    vec3 scale{1.0f, 1.0f, 1.0f};
    bool castShadow{true};

    [[nodiscard]] glm::mat4 modelMatrix() const noexcept {
        return glm::translate(glm::mat4{1.0f}, position.native()) *
               glm::mat4_cast(rotation) *
               glm::scale(glm::mat4{1.0f}, scale.native());
    }
};

} // namespace Engine
