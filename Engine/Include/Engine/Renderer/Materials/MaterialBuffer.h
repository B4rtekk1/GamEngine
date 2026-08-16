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
};

} // namespace Engine
