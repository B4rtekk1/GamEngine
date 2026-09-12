#pragma once

#include "Engine/Math/Math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Engine {

/** Shape of the volume in which a local reflection capture contributes. */
enum class ReflectionProbeShape : std::uint8_t { Box, Sphere };

/**
 * Authoring data for a local specular-IBL capture.
 *
 * The component is deliberately renderer-independent.  The entity transform
 * supplies the probe origin; @ref extents is a local half-size for a box or
 * the radius (x component) for a sphere.  A renderer can therefore upload
 * this exact data to a per-frame probe table and replace its cubemap handle
 * whenever the capture is baked or updated at runtime.
 */
struct ReflectionProbeComponent final {
    ReflectionProbeShape shape{ReflectionProbeShape::Box};
    Vec3 extents{5.0F, 3.0F, 5.0F};
    float blendDistance{1.0F};
    std::int32_t priority{0};
    bool enabled{true};
    bool boxProjection{true};

    /** Returns this probe's smooth influence at a world-space point. */
    [[nodiscard]] float influence(const Vec3& probePosition,
                                  const Vec3& worldPosition) const noexcept {
        if (!enabled) return 0.0F;
        const float safeBlend = std::max(blendDistance, 1.0e-4F);
        const Vec3 delta{std::abs(worldPosition.x() - probePosition.x()),
                         std::abs(worldPosition.y() - probePosition.y()),
                         std::abs(worldPosition.z() - probePosition.z())};
        if (shape == ReflectionProbeShape::Sphere) {
            const float radius = std::max(extents.x(), 1.0e-4F);
            const float distance = std::sqrt(delta.x() * delta.x() +
                                             delta.y() * delta.y() +
                                             delta.z() * delta.z());
            return std::clamp((radius - distance) / safeBlend, 0.0F, 1.0F);
        }
        const float distanceToBoundary = std::min({std::max(extents.x(), 0.0F) - delta.x(),
                                                   std::max(extents.y(), 0.0F) - delta.y(),
                                                   std::max(extents.z(), 0.0F) - delta.z()});
        return std::clamp(distanceToBoundary / safeBlend, 0.0F, 1.0F);
    }
};

/** A GPU-ready choice of the two local probes that may blend at a point. */
struct ReflectionProbeSelection final {
    static constexpr std::uint32_t InvalidProbe = std::numeric_limits<std::uint32_t>::max();
    std::array<std::uint32_t, 2> indices{InvalidProbe, InvalidProbe};
    std::array<float, 2> weights{0.0F, 0.0F};

    /**
     * Inserts a candidate in descending (priority, influence) order.  Keeping
     * exactly two candidates makes overlaps stable without turning local IBL
     * into an unbounded per-pixel loop.
     */
    void consider(const std::uint32_t index, const std::int32_t priority,
                  const float influence) noexcept {
        if (influence <= 0.0F) return;
        if (indices[0] == InvalidProbe || priority > priorities_[0] ||
            (priority == priorities_[0] && influence > weights[0])) {
            indices[1] = indices[0]; weights[1] = weights[0];
            priorities_[1] = priorities_[0];
            indices[0] = index; weights[0] = influence;
            priorities_[0] = priority;
        } else if (indices[1] == InvalidProbe || priority > priorities_[1] ||
                   (priority == priorities_[1] && influence > weights[1])) {
            indices[1] = index; weights[1] = influence; priorities_[1] = priority;
        }
    }

private:
    std::array<std::int32_t, 2> priorities_{std::numeric_limits<std::int32_t>::min(),
                                            std::numeric_limits<std::int32_t>::min()};
};

} // namespace Engine
