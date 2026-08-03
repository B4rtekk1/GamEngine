#pragma once

#include <glm/glm.hpp>

// API-agnostic vertex data used by meshes throughout the renderer.
struct Vertex {
    glm::vec3 position;
    glm::vec3 color;
    glm::vec2 texCoord;
};
