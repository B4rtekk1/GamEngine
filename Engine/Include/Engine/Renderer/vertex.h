#pragma once

#include "../Math/vec3.h"
#include "../Math/vec2.h"

#include <glm/glm.hpp>

namespace Engine {

// API-agnostic vertex data used by meshes throughout the renderer.
struct Vertex {
    vec3 position;
    vec3 color;
    vec2 texCoord;
};

} // namespace Engine
