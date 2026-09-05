#pragma once

#include "Engine/Math/Color.h"

#include <cstdint>
#include <array>

namespace Engine {
    enum class AlphaMode : std::uint8_t { Opaque, Mask, Blend };
    // All runtime normal maps use the glTF/OpenGL (+Y) convention.  The
    // cooker converts DirectX assets before they reach this structure.
    enum class NormalConvention : std::uint8_t { OpenGL, DirectX };
    enum class MaterialShadingModel : std::uint8_t { Standard, Foliage };

    // Values follow the metallic/roughness workflow used by glTF.
    struct PBRMaterial {
        Math::Color baseColor = Math::Color::white();
        float metallic{0.0F};
        float roughness{0.55F}; //NOLINT
        float aoStrength{1.0F};
        std::int32_t baseColorTexture{-1};
        std::int32_t metallicRoughnessTexture{-1};
        std::int32_t normalTexture{-1};
        std::int32_t aoTexture{-1};
        std::int32_t opacityTexture{-1};
        std::int32_t translucencyTexture{-1};
        std::int32_t displacementTexture{-1};
        std::int32_t emissiveTexture{-1};
        std::int32_t specularTexture{-1};
        // glTF normalTexture.scale; zero explicitly disables normal-map detail.
        float normalScale{1.0F};
        AlphaMode alphaMode{AlphaMode::Opaque};
        // glTF `doubleSided`: foliage and thin cards must remain visible from
        // either side. The fragment shader flips their shading normal per face.
        bool doubleSided{false};
        float alphaCutoff{0.5F}; //NOLINT
        float displacementScale{0.0F};
        float displacementOffset{0.0F};
        float translucency{0.0F};
        float specular{0.5F};
        float ior{1.5F};
        NormalConvention normalConvention{NormalConvention::OpenGL};
        MaterialShadingModel shadingModel{MaterialShadingModel::Standard};
        // Terrain uses four albedo layers blended by the vertex splat weights.
        std::array<std::int32_t, 4> terrainLayerTextures{-1, -1, -1, -1};
        bool terrainLayered{false};
    };
} // namespace Engine
