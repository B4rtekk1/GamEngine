#pragma once

#include "Engine/Renderer/Geometry/Vertex.h"

#include <glm/gtc/packing.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

/**
 * @file GpuVertex.h
 * @brief Compact vertex representation uploaded to GPU-only mesh buffers.
 */

namespace Engine {
    /**
     * @brief GPU vertex layout used by all mesh graphics pipelines.
     *
     * Positions deliberately remain full precision. Normals and tangents use
     * A2B10G10R10_SNORM_PACK32, UV streams use two IEEE-754 half floats, and
     * the RGB vertex colour uses RGBA8_UNORM (with an opaque alpha channel).
     * Material index remains a 32-bit value because shaders index material
     * buffers with it. Consequently the complete layout is 36 bytes; a
     * 32-byte version cannot retain that independent 32-bit material index.
     */
    struct GpuVertex {
        float px{};
        float py{};
        float pz{};
        std::uint32_t normal{};
        std::uint32_t tangent{};
        std::uint32_t texCoord{};
        std::uint32_t texCoord1{};
        std::uint32_t color{};
        std::uint32_t materialIndex{};

        [[nodiscard]] static GpuVertex pack(const Vertex& vertex) noexcept {
            return {
                .px = vertex.position.x(),
                .py = vertex.position.y(),
                .pz = vertex.position.z(),
                .normal = glm::packSnorm3x10_1x2(glm::vec4{vertex.normal.native(), 1.0F}),
                .tangent = glm::packSnorm3x10_1x2(vertex.tangent.native()),
                .texCoord = glm::packHalf2x16(vertex.texCoord.native()),
                .texCoord1 = glm::packHalf2x16(vertex.texCoord1.native()),
                .color = glm::packUnorm4x8(glm::vec4{vertex.color.native(), 1.0F}),
                .materialIndex = vertex.materialIndex,
            };
        }
    };

    static_assert(std::is_trivially_copyable_v<GpuVertex>);
    static_assert(sizeof(GpuVertex) == 36);
    static_assert(offsetof(GpuVertex, normal) == 12);
    static_assert(offsetof(GpuVertex, materialIndex) == 32);
} // namespace Engine
