#pragma once

#include <cmath>
#include <cstdint>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

namespace Engine {
    // Reuse depth only when the projection changes by whole virtual pages.
    // In particular, even a small depth/basis change requires rerasterization.
    inline bool vsmClipPageShift(const glm::mat4& previous, const glm::mat4& current,
                                 std::uint32_t pagesPerAxis,
                                 std::int32_t& shiftX, std::int32_t& shiftY) noexcept {
        for (glm::length_t column = 0; column < 4; ++column)
            for (glm::length_t row = 0; row < 4; ++row) {
                if (!std::isfinite(previous[column][row]) || !std::isfinite(current[column][row]))
                    return false;
                if (column == 3 && (row == 0 || row == 1)) continue;
                if (previous[column][row] != current[column][row]) return false;
            }
        if (previous[3][3] == 0.0F || current[3][3] == 0.0F) return false;
        const glm::vec2 delta = (glm::vec2(current[3]) / current[3][3] -
                                 glm::vec2(previous[3]) / previous[3][3]) *
                                (0.5F * static_cast<float>(pagesPerAxis));
        // Out-of-range shifts cannot retain any page and must not be cast to int.
        if (!std::isfinite(delta.x) || !std::isfinite(delta.y) ||
            std::abs(delta.x) >= pagesPerAxis || std::abs(delta.y) >= pagesPerAxis)
            return false;
        shiftX = static_cast<std::int32_t>(std::round(delta.x));
        shiftY = static_cast<std::int32_t>(std::round(delta.y));
        return std::abs(delta.x - shiftX) < 1.0e-3F && std::abs(delta.y - shiftY) < 1.0e-3F;
    }

    // Keep the admitted resident working set stable under oversubscription.
    // Cycling misses ahead of residents causes endless eviction and flicker.
    inline bool vsmRequestBefore(std::uint32_t left, std::uint32_t right,
                                 bool leftResident, bool rightResident,
                                 bool leftDeferred, bool rightDeferred,
                                 std::uint32_t pagesPerLevel) noexcept {
        if (left / pagesPerLevel != right / pagesPerLevel)
            return left / pagesPerLevel < right / pagesPerLevel;
        if (leftResident != rightResident) return leftResident;
        if (leftDeferred != rightDeferred) return leftDeferred;
        return left < right;
    }

    inline bool vsmRefreshBefore(std::uint32_t left, std::uint32_t right,
                                 std::uint64_t leftRendered, std::uint64_t rightRendered,
                                 std::uint32_t leftLevel, std::uint32_t rightLevel) noexcept {
        if (leftRendered != rightRendered) return leftRendered < rightRendered;
        if (leftLevel != rightLevel) return leftLevel > rightLevel;
        return left < right;
    }
}
