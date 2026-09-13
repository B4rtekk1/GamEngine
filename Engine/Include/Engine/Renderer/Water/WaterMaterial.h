#pragma once

#include "Engine/Math/Math.h"

#include <array>
#include <cstdint>

namespace Engine {
    struct WaterGerstnerWave final {
        Vec2 direction{1.0F, 0.0F};
        float amplitude{0.1F};
        float wavelength{5.0F};
        float speed{1.0F};
        float steepness{0.25F};
    };

    [[nodiscard]] inline std::array<WaterGerstnerWave, 8> defaultWaterWaves() {
        return {{{{ 1.00F,  0.10F}, 0.60F, 22.0F, 1.20F, 0.32F},
                 {{ 0.65F,  0.76F}, 0.18F,  5.0F, 2.00F, 0.25F},
                 {{-0.34F,  0.94F}, 0.10F,  3.1F, 2.60F, 0.22F},
                 {{ 0.91F, -0.41F}, 0.07F,  1.8F, 3.30F, 0.18F},
                 {{-0.82F, -0.57F}, 0.03F,  0.7F, 4.20F, 0.12F},
                 {{ 0.18F, -0.98F}, 0.025F, 0.52F,4.80F, 0.10F},
                 {{-0.96F,  0.27F}, 0.04F,  1.1F, 3.80F, 0.14F},
                 {{ 0.48F,  0.88F}, 0.12F,  7.5F, 1.70F, 0.20F}}};
    }
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
        std::array<WaterGerstnerWave, 8> waves{defaultWaterWaves()};
        std::uint32_t waveCount{8};
    };
} // namespace Engine
