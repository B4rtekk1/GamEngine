#pragma once

#include "Engine/Renderer/Geometry/Mesh.h"
#include "Engine/Renderer/Culling/CullingTypes.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace Engine::Culling {
    /** std430 record shared by meshlet_culling.slang and the mesh shader. */
    struct alignas(16) GpuMeshlet final {
        // vertexOffset, vertexCount, triangleOffset, triangleCount
        glm::uvec4 range{};
        // xyz local-space center, w sphere radius
        glm::vec4 bounds{};
        // xyz cone axis, w cosine cutoff; cutoff < -1 disables the cone test.
        glm::vec4 cone{};
        // x material index, remaining components are reserved for LOD/streaming.
        glm::uvec4 material{};
    };
    static_assert(sizeof(GpuMeshlet) == 64);

    /** Matches MeshletCullUniforms in meshlet_culling.slang. */
    struct alignas(16) MeshletCullUniforms final {
        GPUMat4 viewProjection{};
        float cameraX{};
        float cameraY{};
        float cameraZ{};
        std::uint32_t meshletCount{};
        GPUVec4 frustumPlanes[6]{};
    };
    static_assert(sizeof(MeshletCullUniforms) == 176);

    /**
     * Append one mesh's mesh-shader payload to global GPU-ready arrays.
     * Vertex references are translated to the renderer geometry heap by
     * @p firstVertex. The function is intentionally API-agnostic so upload
     * code can use the result with any Vulkan buffer allocator.
     */
    [[nodiscard]] bool appendMeshletPayload(const Mesh& mesh, std::uint32_t firstVertex,
                                            std::vector<GpuMeshlet>& meshlets,
                                            std::vector<std::uint32_t>& vertices,
                                            std::vector<std::uint32_t>& triangles) noexcept;
} // namespace Engine::Culling
