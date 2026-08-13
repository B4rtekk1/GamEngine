#pragma once

#include "Engine/Math/Vec3.h"
#include "Engine/Math/Vec2.h"

#include <glm/glm.hpp>

namespace Engine {

// API-agnostic vertex data used by meshes throughout the renderer.
struct Vertex {
    Vec3 position;
    Vec3 color;
    Vec2 texCoord;
};

} // namespace Engine
