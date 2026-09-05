#pragma once

#include <glm/glm.hpp>
#include <cstdint>

namespace Engine {
    // Vulkan guarantees at least 16 sampled images in a fragment stage.
    inline constexpr std::uint32_t MaxMaterialTextures = 16;

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
    };
} // namespace Engine
