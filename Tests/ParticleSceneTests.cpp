#include <Engine/ECS/Components/ParticleEmitterComponent.h>
#include <Engine/ECS/Components/SmokeEmitterComponent.h>
#include <Engine/Scene/Scene.h>

class TestParticleScene final : public Engine::Scene {
public:
    using Engine::Scene::registry;
    using Engine::Scene::setParticleEmitter;
    using Engine::Scene::setParticleEntity;
};

int main() {
    using namespace Engine;

    TestParticleScene scene;
    if (scene.isParticleScene() || scene.particleEntity() != NullEntity) return 1;

    const Entity smokeEntity = scene.registry().create();
    Particles::SmokeEmitter smoke;
    smoke.spawnRate = 6.0f;
    scene.registry().add<SmokeEmitterComponent>(smokeEntity, SmokeEmitterComponent{.emitter = smoke});
    scene.setParticleEntity(smokeEntity);
    scene.setParticleEmitter(smoke);
    if (!scene.isParticleScene() || scene.particleEntity() != smokeEntity ||
        scene.particleEmitter().spawnRate != 6.0f) return 2;

    scene.registry().destroy(smokeEntity);
    if (scene.particleEntity() != NullEntity || scene.isParticleScene()) return 3;

    const Entity genericEntity = scene.registry().create();
    Particles::ParticleEmitter emitter;
    emitter.spawnRate = 42.0f;
    scene.registry().add<ParticleEmitterComponent>(genericEntity,
        ParticleEmitterComponent{.emitter = emitter});
    if (scene.particleEntity() != genericEntity || !scene.isParticleScene()) return 4;
    return 0;
}
