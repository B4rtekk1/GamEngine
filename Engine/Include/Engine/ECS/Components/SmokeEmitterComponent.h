#pragma once

#include "Engine/Renderer/Particles/ParticleTypes.h"

namespace Engine {
    /** Marks an entity as a physically-aware smoke source. */
    struct SmokeEmitterComponent final {
        Particles::SmokeEmitter emitter;
    };
} // namespace Engine
