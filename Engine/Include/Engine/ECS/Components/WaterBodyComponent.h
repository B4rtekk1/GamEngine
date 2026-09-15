#pragma once

#include "Engine/Renderer/Water/WaterMaterial.h"

#include <array>
#include <cstdint>
#include <vector>

namespace Engine {
    enum class WaterBodyType : std::uint8_t { Ocean, Lake, River };

    /** Geometry representation used by the water renderer. Auto is resolved from measured cost. */
    enum class WaterGeometryMode : std::uint8_t { Auto, AuthoredMesh, SplinePages, VirtualClipmap };
    /** Optical shading path. Budgeted uses tile classification and adaptive ray/shading work. */
    enum class WaterShadingMode : std::uint8_t { Auto, Simple, Budgeted };
    /** Persistent world-space simulation state. */
    enum class WaterStateMode : std::uint8_t { None, Sparse };
    /**
     * Optional high-level preset. Auto keeps geometry/shading/state independent;
     * the forced presets are useful for deterministic benchmarking and authoring.
     */
    enum class WaterExecutionTier : std::uint8_t { Auto, Simple, VirtualLite, FullVirtual };

    struct WaterExecutionPolicy final {
        WaterExecutionTier tier{WaterExecutionTier::Auto};
        WaterGeometryMode geometry{WaterGeometryMode::Auto};
        WaterShadingMode shading{WaterShadingMode::Auto};
        WaterStateMode state{WaterStateMode::Sparse};
        float switchMargin{0.15F};
        std::uint32_t minGeometryHoldFrames{60};
        std::uint32_t minShadingHoldFrames{8};
    };

    using GerstnerWave = WaterGerstnerWave;

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
        float refractionStrength{0.025F};
        float normalStrength{1.0F};
        float foamIntensity{0.5F};
        float foamThreshold{0.7F};
        float maxDepth{20.0F};
        std::int32_t normalMap{-1};
        std::int32_t foamTexture{-1};
        std::int32_t flowMap{-1};
        bool enableSSR{true};
        bool enableCaustics{false};
        bool enableUnderwater{true};
        WaterExecutionPolicy execution{};
        std::array<GerstnerWave, 8> waves{defaultWaterWaves()};
        std::uint32_t waveCount{8};
        std::vector<Vec3> lakeBoundary;
        std::vector<RiverSplinePoint> riverSpline;
    };
} // namespace Engine
