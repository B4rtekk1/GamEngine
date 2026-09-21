#pragma once

#include "Engine/Math/Color.h"
#include "Engine/Math/Vec3.h"

namespace Engine::Particles {
    struct ParticleEmitter {
        Vec3 position;
        Vec3 minVelocity{-1.0F, 1.0F, -1.0F};
        Vec3 maxVelocity{1.0F, 4.0F, 1.0F};
        Color color{1.0F, 1.0F, 1.0F, 1.0F};
        float minLifeTime = 1.0F;
        float maxLifeTime = 2.0F;
        float minSize = 0.04F;
        float maxSize = 0.12F;
        float spawnRate = 200.0F;
        float accumulator = 0.0F;
    };

    inline constexpr float DefaultSmokeBuoyancy = 2.25F;

    // NOLINTBEGIN(readability-magic-numbers)
    struct SmokeEmitter final : ParticleEmitter {
        float buoyancy = DefaultSmokeBuoyancy;
        float drag = 0.68F;
        float turbulence = 0.30F;
        float collisionRadius = 0.10F;

        SmokeEmitter() {
            minVelocity = {-0.24F, 0.45F, -0.24F};
            maxVelocity = {0.24F, 1.05F, 0.24F};
            color = {0.18F, 0.20F, 0.23F, 0.19F};
            minLifeTime = 5.5F;
            maxLifeTime = 9.0F;
            minSize = 0.28F;
            maxSize = 0.88F;
            spawnRate = 260.0F;
        }
    };
    // NOLINTEND(readability-magic-numbers)
}
