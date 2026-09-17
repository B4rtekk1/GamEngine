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

    /** GPU form of a baked meshlet-hierarchy node.  Child and meshlet ranges
     * are rebased to the global payload streams during scene upload. */
    struct alignas(16) GpuMeshletCluster final {
        glm::vec4 bounds{}; // xyz local-space center, w conservative radius
        glm::vec4 cone{};   // optional conservative cone; w < -1 disables it
        glm::uvec4 range{}; // firstChild, childCount, firstMeshlet, meshletCount
    };
    static_assert(sizeof(GpuMeshletCluster) == 48);

    /** One fine-grained visibility result. Meshlet IDs address the global
     * meshlet payload; instance IDs address the GPU-scene instance table. */
    struct VisibleMeshlet final {
        std::uint32_t instanceId{};
        std::uint32_t meshletId{};
    };
    static_assert(sizeof(VisibleMeshlet) == 8);

    /** Matches MeshletCullUniforms in meshlet_culling.slang. */
    struct alignas(16) MeshletCullUniforms final {
        GPUMat4 viewProjection{};
        GPUMat4 occlusionViewProjection{};
        float cameraX{};
        float cameraY{};
        float cameraZ{};
        std::uint32_t meshletCount{};
        GPUVec4 frustumPlanes[6]{};
        float viewportWidth{};
        float viewportHeight{};
        float depthBias{};
        std::uint32_t hiZMipCount{};
        std::uint32_t enableOcclusionCulling{};
        std::uint32_t cameraCut{};
        std::uint32_t reserved[2]{};
    };
    static_assert(sizeof(MeshletCullUniforms) == 272);

    /**
     * Append one mesh's mesh-shader payload to global GPU-ready arrays.
     * Vertex references are translated to the renderer geometry heap by
     * @p firstVertex. The function is intentionally API-agnostic so upload
     * code can use the result with any Vulkan buffer allocator.
     */
    [[nodiscard]] bool appendMeshletPayload(const Mesh& mesh, std::uint32_t firstVertex,
                                            std::vector<GpuMeshlet>& meshlets,
                                            std::vector<std::uint32_t>& vertices,
                                            std::vector<std::uint32_t>& triangles);

    /** Appends the baked hierarchy using global child and meshlet indices.
     * Returns false when the source hierarchy cannot be represented safely. */
    [[nodiscard]] bool appendMeshletClusterPayload(const Mesh& mesh,
                                                   std::uint32_t firstMeshlet,
                                                   std::vector<GpuMeshletCluster>& clusters);
} // namespace Engine::Culling
