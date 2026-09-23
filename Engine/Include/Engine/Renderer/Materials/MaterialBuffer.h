#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <array>

namespace Engine {
    // Global bindless texture table.  Indices in PBRMaterial refer directly
    // to this table and are never rebound per material.
    inline constexpr std::uint32_t MaxMaterialTextures = 4096;

    /** @brief Shader representation of one PBR material. */
    struct alignas(16) GPUMaterialData {
        glm::vec4 baseColorMetallic{};
        glm::vec4 roughnessAmbientOcclusion{};
        glm::ivec4 textureIndices{-1};
        glm::ivec4 terrainLayerTextures{-1};
        // AO, opacity, translucency, displacement.  The final component is
        // reserved for cooker-added maps without changing the first block.
        glm::ivec4 auxiliaryTextureIndices{-1};
        // normalScale, translucency strength, displacement scale, specular.
        glm::vec4 extensionScalars{};
        // emissive, KHR_materials_specular strength, specular colour, reserved.
        glm::ivec4 extensionTextureIndices{-1};
        // KHR_materials_specular colour factor in linear RGB.
        glm::vec4 specularColor{};
        // Linear RGB emission colour and unconstrained HDR intensity.
        glm::vec4 emissiveColorIntensity{};
        glm::ivec4 textureCoordinateSets0{};
        glm::ivec4 textureCoordinateSets1{};
        glm::ivec4 textureCoordinateSets2{};
        // Per-slot affine UV transform. textureTransforms stores (m00, m01,
        // offsetX, offsetY); textureTransformRows1 packs (m10, m11).
        std::array<glm::vec4, 10> textureTransforms{};
        std::array<glm::vec4, 6> textureTransformRows1{};
        // Dedicated water block. Kept in the material SSBO so the existing
        // GPU-driven material-index indirection remains intact.
        glm::vec4 waterShallowColorRoughness{};
        glm::vec4 waterDeepColorIor{};
        glm::vec4 waterAbsorptionRefraction{};
        glm::vec4 waterScatteringMaxDepth{};
        glm::vec4 waterFoam{};
        glm::ivec4 waterTextureIndices{-1};
        // xy direction, z amplitude, w wavelength; x speed, y steepness.
        std::array<glm::vec4, 8> waterWaves{};
        std::array<glm::vec4, 8> waterWaveMotion{};
        glm::uvec4 waterWaveCount{};
    };
} // namespace Engine
