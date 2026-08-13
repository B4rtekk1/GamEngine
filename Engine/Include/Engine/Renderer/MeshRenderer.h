#pragma once

#include "mesh.h"

#include <cstdint>

namespace Engine {

// ECS component describing geometry submitted by an entity.
struct MeshRenderer {
    Mesh mesh;
    bool castShadow{true};

    // Set when the scene geometry is uploaded to the shared GPU index buffer.
    uint32_t firstIndex{0};
};

} // namespace Engine
