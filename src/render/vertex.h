#pragma once

#include "../core/Vec3.h"

#include <glm/glm.hpp>

// API-agnostic vertex data used by meshes throughout the renderer.
struct Vertex {
    Vec3 position;
    Vec3 color;
    glm::vec2 texCoord;
};
