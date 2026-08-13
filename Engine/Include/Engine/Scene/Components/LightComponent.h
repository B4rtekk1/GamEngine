#pragma once

#include "Engine/Math/Vec3.h"

namespace Engine {

    enum class LightType {
        Directional,
        Point,
        Spot,
    };

    struct LightComponent {
        LightType type = LightType::Directional;

        Vec3 color{1.0f, 1.0f, 1.0f};
        float intensity = 1.0f;

        bool enabled = true;
        bool castShadows = true;
    };
}