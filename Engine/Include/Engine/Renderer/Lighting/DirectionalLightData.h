#pragma once

#include <Engine/Math/Color.h>
#include <Engine/Math/Vec4.h>

namespace Engine {
    struct alignas(16) DirectionalLightGPU {
        Vec4 directionIntensity;
        Math::Color color;
    };

    static_assert(
        sizeof(DirectionalLightGPU) == 32,
        "DirectionalLightGPU must be 32 bytes in size for proper alignment in uniform buffers.");
}
