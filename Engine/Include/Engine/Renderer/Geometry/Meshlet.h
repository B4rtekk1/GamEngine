#pragma once

#include "Engine/Math/Vec3.h"

#include <cstdint>
#include <vector>

namespace Engine {
    /**
     * GPU-friendly description of a small, independently cullable portion of
     * a mesh. Offsets address Mesh::meshletVertices and
     * Mesh::meshletTriangles respectively. A triangle is packed as three
     * eight-bit indices into the meshlet's local vertex list.
     */
    struct Meshlet final {
        std::uint32_t vertexOffset{};
        std::uint32_t vertexCount{};
        std::uint32_t triangleOffset{};
        std::uint32_t triangleCount{};
        Vec3 center{};
        float radius{};
        Vec3 coneAxis{};
        /// Cosine of the widest normal angle. Values below -1 disable cone culling.
        float coneCutoff{-1.0F};
        /// Mesh shaders must not combine primitives using different materials.
        std::uint32_t materialIndex{};
        // std430 rounds an array element containing vec3 fields to 16-byte
        // alignment. Keep the serialized C++ record at that same 64-byte stride.
        std::uint32_t reserved[3]{};
    };

    static_assert(sizeof(Meshlet) == 64, "Meshlet is a serialized GPU ABI");

    struct MeshletBuildOptions final {
        static constexpr std::uint32_t MaxVertices = 64;
        static constexpr std::uint32_t MaxTriangles = 126;
        std::uint32_t maxVertices{MaxVertices};
        std::uint32_t maxTriangles{MaxTriangles};
    };

    class Mesh;

    /**
     * Partitions triangle geometry into meshlets. The implementation keeps
     * primitive order stable and material boundaries intact; it is deliberately
     * dependency-free so cooked assets work on every developer machine.
     * meshoptimizer can later replace only this implementation without changing
     * the cooked or GPU-facing data contract.
     */
    [[nodiscard]] bool build_meshlets(Mesh& mesh,
                                      MeshletBuildOptions options = {}) noexcept;
} // namespace Engine
