#pragma once

#include "Engine/Math/Math.h"
namespace Engine {

    enum class LightType {
        Directional,
        Point,
        Spot,
    };

    struct LightComponent {
        LightType type = LightType::Directional;

        Math::Color color{1.0f, 1.0f, 1.0f};
        float intensity = 1.0f;

        bool enabled = true;
        bool castShadows = true;
    };
}