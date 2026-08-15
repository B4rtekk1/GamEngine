#pragma once

#include "Engine/Renderer/Geometry/Mesh.h"
#include "Engine/Renderer/Materials/PBRMateial.h"

#include <cstdint>
#include <limits>
#include <memory>

namespace Engine {

/**
 * @brief ECS component that makes an entity renderable as a mesh.
 *
 * Mesh data is shared and immutable, while material and rendering flags remain
 * per entity. GPU bookkeeping fields are owned by the renderer and should not
 * be serialized as scene state.
 */
struct MeshRendererComponent final {
    /// Geometry to submit. The same mesh may be referenced by many entities.
    std::shared_ptr<const Mesh> mesh;

    /// Per-entity material parameters for the PBR forward pass.
    PBRMaterial material{};

    /// Whether this mesh contributes to the shadow map.
    bool castShadow{true};

    /// Offset into the renderer's shared index buffer; assigned during upload.
    uint32_t firstIndex{0};

    /// Renderer-owned slot in the occlusion-query pool.
    uint32_t occlusionQueryIndex{std::numeric_limits<uint32_t>::max()};

    [[nodiscard]] bool hasMesh() const noexcept {
        return mesh != nullptr && !mesh->empty();
    }
};

} // namespace Engine
