#pragma once

#include "Engine/Math/Math.h"

namespace Engine {
    enum class LightType:uint8_t {
        Directional,
        Point,
        Spot,
    };

    struct LightComponent {
        LightType type = LightType::Directional;

        Math::Color color{1.0F, 1.0F, 1.0F};
        float intensity = 1.0F;

        bool enabled = true;
        bool castShadows = true;
    };
}
