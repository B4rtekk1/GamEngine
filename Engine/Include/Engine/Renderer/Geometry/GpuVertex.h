#pragma once

#include "Engine/Renderer/Geometry/Vertex.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <glm/geometric.hpp>
#include <glm/gtc/packing.hpp>
#include <type_traits>

/**
 * @file GpuVertex.h
 * @brief Compact vertex representation uploaded to GPU-only mesh buffers.
 */

namespace Engine {
/**
 * @brief GPU vertex layout used by all mesh graphics pipelines (32 bytes).
 *
 * Positions deliberately remain full precision. The normal and tangent share
 * one 32-bit tangent frame (see packTangentFrame), UV streams use two IEEE-754
 * half floats, and the RGB vertex colour uses RGBA8_UNORM (with an opaque
 * alpha channel). Material index remains a 32-bit value because shaders index
 * material buffers with it.
 */
struct GpuVertex {
    float px{};
    float py{};
    float pz{};
    std::uint32_t tangentFrame{};
    std::uint32_t texCoord{};
    std::uint32_t texCoord1{};
    std::uint32_t color{};
    std::uint32_t materialIndex{};

    /**
     * Packs a unit normal and a tangent (xyz direction, w handedness):
     *  - bits  0-21: octahedral normal, two 11-bit components (q - 1023) / 1023;
     *  - bits 22-30: tangent angle around the decoded normal, 512 steps over
     *                [-pi, pi), in the basis of tangentFrameBasis();
     *  - bit  31:    set when the bitangent handedness (tangent.w) is negative.
     * Must match Engine/Shaders/Common/VertexFrame.slang.
     */
    [[nodiscard]] static std::uint32_t packTangentFrame(const glm::vec3& normal, const glm::vec4& tangent) noexcept {
        glm::vec3 n = normal;
        const float l1 = std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
        n = l1 > 1.0e-20F ? n / l1 : glm::vec3{0.0F, 0.0F, 1.0F};
        glm::vec2 e{n.x, n.y};
        if (n.z < 0.0F) {
            e = {(1.0F - std::abs(n.y)) * (n.x >= 0.0F ? 1.0F : -1.0F),
                 (1.0F - std::abs(n.x)) * (n.y >= 0.0F ? 1.0F : -1.0F)};
        }
        // Of the four grid points around e, keep the one decoding closest
        // to the normal (precise octahedral encoding).
        const glm::vec3 target = l1 > 1.0e-20F ? glm::normalize(normal) : glm::vec3{0.0F, 0.0F, 1.0F};
        const auto cell = [](const float value) {
            return std::clamp(static_cast<long>(std::floor(value * 1023.0F)) + 1023L, 0L, 2045L);
        };
        const long baseX = cell(e.x);
        const long baseY = cell(e.y);
        std::uint32_t frame = 0;
        float best = -2.0F;
        for (long dy = 0; dy < 2; ++dy) {
            for (long dx = 0; dx < 2; ++dx) {
                const auto candidate = static_cast<std::uint32_t>(baseX + dx) |
                                       (static_cast<std::uint32_t>(baseY + dy) << 11U);
                const float score = glm::dot(unpackNormal(candidate), target);
                if (score > best) {
                    best = score;
                    frame = candidate;
                }
            }
        }

        // The angle is measured around the normal the GPU will decode.
        const glm::vec3 decoded = unpackNormal(frame);
        glm::vec3 b1{};
        glm::vec3 b2{};
        tangentFrameBasis(decoded, b1, b2);
        const glm::vec3 t = glm::vec3{tangent} - decoded * glm::dot(decoded, glm::vec3{tangent});
        std::uint32_t angle = 256U; // angle 0: b1, for degenerate tangents
        if (glm::dot(t, t) > 1.0e-24F) {
            constexpr float Pi = 3.14159265358979F;
            const float radians = std::atan2(glm::dot(t, b2), glm::dot(t, b1));
            angle = static_cast<std::uint32_t>(std::lround((radians + Pi) * (512.0F / (2.0F * Pi)))) & 511U;
        }
        frame |= angle << 22U;
        if (tangent.w < 0.0F)
            frame |= 0x80000000U;
        return frame;
    }

    [[nodiscard]] static glm::vec3 unpackNormal(const std::uint32_t frame) noexcept {
        const glm::vec2 e = (glm::vec2{static_cast<float>(frame & 2047U), static_cast<float>((frame >> 11U) & 2047U)} -
                             1023.0F) /
                            1023.0F;
        glm::vec3 n{e.x, e.y, 1.0F - std::abs(e.x) - std::abs(e.y)};
        const float t = std::clamp(-n.z, 0.0F, 1.0F);
        n.x += n.x >= 0.0F ? -t : t;
        n.y += n.y >= 0.0F ? -t : t;
        return glm::normalize(n);
    }

    [[nodiscard]] static glm::vec4 unpackTangent(const std::uint32_t frame) noexcept {
        glm::vec3 b1{};
        glm::vec3 b2{};
        tangentFrameBasis(unpackNormal(frame), b1, b2);
        constexpr float Pi = 3.14159265358979F;
        const float angle = static_cast<float>((frame >> 22U) & 511U) * (2.0F * Pi / 512.0F) - Pi;
        return {std::cos(angle) * b1 + std::sin(angle) * b2, (frame & 0x80000000U) != 0U ? -1.0F : 1.0F};
    }

    /// Duff et al. 2017, "Building an Orthonormal Basis, Revisited".
    static void tangentFrameBasis(const glm::vec3& n, glm::vec3& b1, glm::vec3& b2) noexcept {
        const float s = n.z >= 0.0F ? 1.0F : -1.0F;
        const float a = -1.0F / (s + n.z);
        const float b = n.x * n.y * a;
        b1 = {1.0F + s * n.x * n.x * a, s * b, -s * n.x};
        b2 = {b, s + n.y * n.y * a, -n.y};
    }

    [[nodiscard]] static GpuVertex pack(const Vertex& vertex) noexcept {
        return {
            .px = vertex.position.x(),
            .py = vertex.position.y(),
            .pz = vertex.position.z(),
            .tangentFrame = packTangentFrame(vertex.normal.native(), vertex.tangent.native()),
            .texCoord = glm::packHalf2x16(vertex.texCoord.native()),
            .texCoord1 = glm::packHalf2x16(vertex.texCoord1.native()),
            .color = glm::packUnorm4x8(glm::vec4{vertex.color.native(), 1.0F}),
            .materialIndex = vertex.materialIndex,
        };
    }
};

static_assert(std::is_trivially_copyable_v<GpuVertex>);
static_assert(sizeof(GpuVertex) == 32);
static_assert(offsetof(GpuVertex, tangentFrame) == 12);
static_assert(offsetof(GpuVertex, texCoord) == 16);
static_assert(offsetof(GpuVertex, color) == 24);
static_assert(offsetof(GpuVertex, materialIndex) == 28);
} // namespace Engine
