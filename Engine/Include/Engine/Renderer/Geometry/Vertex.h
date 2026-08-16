#pragma once

#include "Engine/Math/Vec3.h"
#include "Engine/Math/Vec2.h"
#include "Engine/Math/Vec4.h"

#include <glm/glm.hpp>
#include <cstdint>

namespace Engine {

// API-agnostic vertex data used by meshes throughout the renderer.
struct Vertex {
    Vec3 position;
    Vec3 color;
    Vec2 texCoord;
    Vec3 normal;
    // xyz is the tangent direction, w is its handedness for the bitangent.
    Vec4 tangent;
    // Index into Mesh::materials. Kept at the end so the existing Vulkan
    // position/color/normal offsets remain stable.
    std::uint32_t materialIndex{0};
};

} // namespace Engine
