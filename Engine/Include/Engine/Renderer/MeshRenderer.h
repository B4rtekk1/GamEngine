#pragma once

#include "mesh.h"

#include <cstdint>
#include <limits>
#include <memory>

namespace Engine {

// ECS component describing geometry submitted by an entity.
struct MeshRenderer {
    // Geometry is immutable and can be shared by many entities.  Keeping a
    // single cube mesh avoids allocating the same vertices and indices for
    // every cube in a scene.
    std::shared_ptr<const Mesh> mesh;
    bool castShadow{true};

    // Set when the scene geometry is uploaded to the shared GPU index buffer.
    uint32_t firstIndex{0};

    // Stable slot used by the renderer's per-object occlusion query pool.
    uint32_t occlusionQueryIndex{std::numeric_limits<uint32_t>::max()};

    [[nodiscard]] bool hasMesh() const noexcept {
        return mesh != nullptr && !mesh->empty();
    }
};

} // namespace Engine
