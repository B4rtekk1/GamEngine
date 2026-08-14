#pragma once

#include <glm/glm.hpp>

namespace Engine {

/** @brief Shader representation of one PBR material. */
struct alignas(16) GPUMaterialData {
    glm::vec4 baseColorMetallic{};
    glm::vec4 roughnessAmbientOcclusion{};
};

} // namespace Engine
