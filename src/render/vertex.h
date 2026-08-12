#pragma once

#include "../core/math/vec3.h"
#include "../core/math/vec2.h"

#include <glm/glm.hpp>

// API-agnostic vertex data used by meshes throughout the renderer.
struct Vertex {
    vec3 position;
    vec3 color;
    vec2 texCoord;
};
