#pragma once

#include <array>
#include <cstdint>

namespace Engine {
    struct DDGIVolume final {
        std::array<std::uint32_t, 3> probeCounts{16, 8, 16};
        float probeSpacing{2.0F};
        std::uint32_t raysPerProbe{64};
        float maxRayDistance{32.0F};
        std::array<float, 3> origin{};
        std::array<std::int32_t, 3> scrollOffset{};
    };
}
