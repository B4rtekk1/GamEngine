#pragma once

#include "Engine/Math/Math.h"

#include <cstdint>

namespace Engine {
    /**
     * Optical and surface parameters for the dedicated Water pipeline.
     *
     * This deliberately lives next to renderer water code rather than in
     * PBRMaterial: water colour comes from transmission through a volume, not
     * from a diffuse base colour.
     */
    struct WaterMaterial final {
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
        float depthFadeDistance{0.4F};
        float maxVisibleDepth{20.0F};
        std::int32_t normalMap{-1};
        std::int32_t foamTexture{-1};
        std::int32_t flowMap{-1};
        bool enableSSR{true};
        bool enableCaustics{false};
        bool enableUnderwater{true};
    };
} // namespace Engine
