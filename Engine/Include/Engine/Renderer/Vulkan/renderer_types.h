#pragma once

#include <cstdint>
#include <limits>

#include <glm/glm.hpp>

#include "Engine/Core/Camera.h"

namespace Engine {
    struct RendererUniformBufferObject {
        Mat4 view;
        Mat4 projection;
        Mat4 lightSpace;
        Vec4 cameraPosition;
        Vec4 lightDirectionIntensity;
        Vec4 lightColor;
        std::uint32_t shadowEnabled{0};
        std::uint32_t materialSlots{1};
        std::uint32_t selectedInstance{std::numeric_limits<std::uint32_t>::max()};
        std::uint32_t materialSlotsPadding{};
    };

    /** Per-instance vertex data for the forward pipelines. */
    struct RendererInstanceData {
        glm::mat4 model{1.0F};
        glm::vec4 normalColumn0{1.0F, 0.0F, 0.0F, 0.0F};
        glm::vec4 normalColumn1{0.0F, 1.0F, 0.0F, 0.0F};
        glm::vec4 normalColumn2{0.0F, 0.0F, 1.0F, 0.0F};
        // xy: local bend direction, z: permanent trample amount.
        glm::vec4 grassDeformation{};
    };
} // namespace Engine
