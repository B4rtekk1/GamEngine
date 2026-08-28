#pragma once

#include "Engine/Renderer/Particles/ParticleSystem.h"

namespace Engine {
    /** ECS data that makes a particle emitter part of the scene hierarchy. */
    struct ParticleEmitterComponent final {
        Particles::ParticleEmitter emitter{};
    };
} // namespace Engine
