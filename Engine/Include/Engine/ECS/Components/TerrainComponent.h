#pragma once

#include "Engine/Renderer/Geometry/Mesh.h"

#include <cstdint>
#include <cstddef>
#include <vector>

namespace Engine {

enum class TerrainSculptMode : std::uint8_t {
    Raise,
    Lower,
    Smooth,
    Flatten,
};

/** Heightmap-backed, editor-sculptable terrain data. */
struct TerrainComponent final {
    static constexpr std::uint32_t DefaultResolution = 33;
    static constexpr std::uint32_t MinimumResolution = 2;
    static constexpr std::uint32_t MaximumResolution = 513;

    std::uint32_t resolution{DefaultResolution};
    float width{100.0F};
    float depth{100.0F};
    float minimumHeight{-10.0F};
    float maximumHeight{10.0F};
    std::vector<float> heights;

    TerrainComponent();
    TerrainComponent(std::uint32_t resolution, float width, float depth,
                     float minimumHeight = -10.0F, float maximumHeight = 10.0F);

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::size_t sampleCount() const noexcept;
    [[nodiscard]] float height(std::uint32_t x, std::uint32_t z) const;
    [[nodiscard]] float sampleHeight(float localX, float localZ) const noexcept;
    [[nodiscard]] Mesh createMesh() const;

    /** Applies one brush sample in local terrain space. */
    bool sculpt(float localX, float localZ, float radius, float amount,
                TerrainSculptMode mode, float flattenHeight = 0.0F);
};

} // namespace Engine
