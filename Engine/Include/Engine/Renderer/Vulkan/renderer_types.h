#pragma once

#include <cstdint>
#include <limits>
#include <array>

#include <glm/glm.hpp>

#include "Engine/Core/Camera.h"
#include "Engine/Renderer/Vulkan/shadow_map.h"

namespace Engine {
    struct RendererUniformBufferObject {
        Mat4 view;
        Mat4 projection;
        // Camera-centred directional-light virtual clipmaps.
        std::array<Mat4, 4> shadowClipMatrices{};
        Vec4 cameraPosition;
        Vec4 lightDirectionIntensity;
        Vec4 lightColor;
        std::uint32_t shadowEnabled{0};
        std::uint32_t materialSlots{1};
        std::uint32_t selectedInstance{std::numeric_limits<std::uint32_t>::max()};
        std::uint32_t materialSlotsPadding{};
    };

    /**
     * Compact per-instance data.  Position, rotation and scale reconstruct
     * the model and normal transforms in the vertex shader; this halves the
     * old matrix-plus-normal representation (128 B -> 64 B).
     */
    struct RendererInstanceData {
        // xyz: world position, w: bit-cast material-table base index.
        glm::vec4 positionMaterial{};
        // Quaternion stored as xyzw.
        glm::vec4 rotation{0.0F, 0.0F, 0.0F, 1.0F};
        // xyz: non-uniform scale, w: local mesh minimum Y for grass bending.
        glm::vec4 scaleBase{1.0F, 1.0F, 1.0F, 0.0F};
        // xyz: bend X, bend Z, trample; w: reciprocal grass mesh height.
        glm::vec4 grassDeformation{};
    };
} // namespace Engine
