#pragma once

#include "Engine/Math/Math.h"

namespace Engine {
    /**
     * Scene-wide procedural wind.  It is sampled once per rendered frame and
     * evaluated in the grass vertex shaders, so it adds no per-blade CPU work.
     */
    struct WindComponent final {
        Vec3 direction{1.0F, 0.0F, 0.0F};
        float strength{0.18F};
        float gustStrength{0.12F};
        float frequency{0.65F};
        // World-space radius of this local wind source.
        float range{12.0F};
        bool enabled{true};
    };
} // namespace Engine
