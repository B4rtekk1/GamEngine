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
} // namespace Engine
