#pragma once

#include "Engine/Math/Math.h"

#include <array>
#include <cstdint>
#include <vector>

namespace Engine {
    enum class WaterBodyType : std::uint8_t { Ocean, Lake, River };

    struct GerstnerWave final {
        Vec2 direction{1.0F, 0.0F};
        float amplitude{0.1F};
        float wavelength{5.0F};
        float speed{1.0F};
        float steepness{0.25F};
    };

    struct RiverSplinePoint final {
        Vec3 position{};
        float width{4.0F};
        float depth{1.0F};
        float flowSpeed{1.0F};
    };

    /**
     * Authoring data for a water surface. WaterSystem owns its generated
     * geometry: a camera-centred clipmap for Ocean, a triangulated boundary
     * for Lake and a ribbon built from riverSpline for River.
     */
    struct WaterBodyComponent final {
        WaterBodyType type{WaterBodyType::Lake};
        Vec3 shallowColor{0.08F, 0.42F, 0.48F};
        Vec3 deepColor{0.01F, 0.07F, 0.16F};
        Vec3 absorptionCoefficient{0.08F, 0.03F, 0.015F};
        Vec3 scatteringCoefficient{0.02F, 0.05F, 0.06F};
        float roughness{0.08F};
        float ior{1.333F};
        float foamIntensity{0.5F};
        float foamThreshold{0.7F};
        float maxDepth{20.0F};
        bool enableSSR{true};
        bool enableCaustics{false};
        bool enableUnderwater{true};
        std::array<GerstnerWave, 8> waves{};
        std::uint32_t waveCount{};
        std::vector<Vec3> lakeBoundary;
        std::vector<RiverSplinePoint> riverSpline;
    };
} // namespace Engine
