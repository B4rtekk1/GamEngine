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

        // Local lights fade smoothly to zero at this distance. It is ignored
        // by directional lights.
        float range = 10.0F;
        // Cone angles in degrees, used by spot lights only. The inner cone is
        // fully lit; the outer cone is the soft edge of the beam.
        float innerConeAngle = 20.0F;
        float outerConeAngle = 30.0F;

        bool enabled = true;
        bool castShadows = true;
        // Selects the sole directional light currently consumed by the forward
        // renderer. It does not control whether this light itself is enabled.
        bool mainLight = false;
    };
}
