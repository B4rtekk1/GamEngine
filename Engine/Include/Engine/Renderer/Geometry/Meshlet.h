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

    /** A node in the baked meshlet hierarchy.  Children and meshlet ranges
     * are local to their owning Mesh.  A node either has children or owns a
     * contiguous leaf range of meshlets. */
    struct MeshletClusterNode final {
        Vec3 center{};
        float radius{};
        float geometricError{};
        std::uint32_t firstChild{};
        std::uint32_t childCount{};
        std::uint32_t firstMeshlet{};
        std::uint32_t meshletCount{};
        std::uint32_t reserved[3]{};
    };
    static_assert(sizeof(MeshletClusterNode) == 48, "MeshletClusterNode is a serialized GPU ABI");

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
                                      MeshletBuildOptions options = {});

    /** Builds a breadth-first hierarchy over already-built meshlets.  Leaves
     * contain at most @p leafMeshlets meshlets and internal nodes have at most
     * @p maxChildren children.  Meshlets must be spatially ordered first. */
    [[nodiscard]] bool build_meshlet_cluster_hierarchy(Mesh& mesh,
                                                        std::uint32_t leafMeshlets = 32U,
                                                        std::uint32_t maxChildren = 8U);

    /**
     * Splits large imported sections into spatially local, contiguous index
     * ranges.  The index stream is reordered only inside each original
     * section, so material ownership and the shared GPU geometry allocation
     * are retained.  Each produced range targets @p targetMeshlets meshlets
     * at the default meshlet triangle limit.
     */
    void subdivide_render_sections(Mesh& mesh,
                                   std::uint32_t targetMeshlets = 32U);
} // namespace Engine
