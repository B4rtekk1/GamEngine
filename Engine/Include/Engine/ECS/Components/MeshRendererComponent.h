#pragma once

#include "Engine/Renderer/Geometry/MeshGpuResource.h"
#include "Engine/Renderer/Materials/Material.h"

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
        MeshHandle mesh;

        /// Per-entity surface material. The renderer resolves its shader to a
        /// pipeline batch; entities never own or bind Vulkan shaders directly.
        Material material{};
        bool materialOverride{false};

        /// Whether this mesh contributes to the shadow map.
        bool castShadow{true};

        /// Optional spatial batch identifier used by GPU culling. Objects with
        /// the same mesh, shadow flag and identifier share one indirect draw.
        uint32_t cullingBatch{0};

        /// Offset into the renderer's shared index buffer; assigned during upload.
        uint32_t firstIndex{0};

        /// Renderer-owned slot in the occlusion-query pool.
        uint32_t occlusionQueryIndex{std::numeric_limits<uint32_t>::max()};

        [[nodiscard]] bool hasMesh() const noexcept {
            // Before the initial upload, the decoded payload establishes that
            // this is drawable. Afterwards its GPU range is authoritative.
            return mesh != nullptr && (mesh.uploaded() ||
                (mesh.hasSourceData() && !mesh->empty()));
        }
    };
} // namespace Engine
