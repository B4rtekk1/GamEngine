#pragma once

#include "Engine/ECS/Components/ColliderComponent.h"
#include "Engine/Core/Transform.h"
#include "Engine/Renderer/Particles/ParticleSystem.h"

namespace Engine {

namespace RendererSceneHelpers {

Particles::ParticleCollider makeParticleCollider(const ColliderComponent& collider,
                                                  const Transform& transform);

} // namespace RendererSceneHelpers
} // namespace Engine
