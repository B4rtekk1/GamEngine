#include <Engine/ECS/Components/ColorPickerComponent.h>
#include <Engine/ECS/Components/ParticleEmitterComponent.h>
#include <Engine/ECS/Components/SmokeEmitterComponent.h>

int main() {
    using namespace Engine;
    using namespace Engine::Particles;

    ParticleEmitter emitter;
    if (emitter.position.x() != 0.0f || emitter.minVelocity.y() != 1.0f ||
        emitter.maxVelocity.y() != 4.0f || emitter.minLifeTime != 1.0f ||
        emitter.maxLifeTime != 2.0f || emitter.minSize != 0.04f ||
        emitter.maxSize != 0.12f || emitter.spawnRate != 200.0f ||
        emitter.accumulator != 0.0f) return 1;

    SmokeEmitter smoke;
    if (smoke.minVelocity.x() != -0.24f || smoke.maxVelocity.y() != 1.05f ||
        smoke.color.a() != 0.19f || smoke.minLifeTime != 5.5f ||
        smoke.maxLifeTime != 9.0f || smoke.minSize != 0.28f ||
        smoke.maxSize != 0.88f || smoke.spawnRate != 260.0f ||
        smoke.buoyancy != 2.25f || smoke.drag != 0.68f ||
        smoke.turbulence != 0.30f || smoke.collisionRadius != 0.10f) return 2;

    ParticleEmitterComponent particleComponent;
    SmokeEmitterComponent smokeComponent;
    if (particleComponent.emitter.spawnRate != 200.0f ||
        smokeComponent.emitter.spawnRate != 260.0f ||
        smokeComponent.emitter.buoyancy != 2.25f) return 3;

    ColorPickerComponent picker;
    if (picker.color.r() != 1.0f || picker.color.g() != 1.0f ||
        picker.color.b() != 1.0f || picker.color.a() != 1.0f) return 4;
    picker.color = Color::from_rgba(0.1f, 0.2f, 0.3f, 0.4f);
    if (picker.color.r() != 0.1f || picker.color.a() != 0.4f) return 5;
    return 0;
}
