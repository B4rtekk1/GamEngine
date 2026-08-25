#include <Engine/ECS/Components/ColorPickerComponent.h>
#include <Engine/ECS/Components/ParticleEmitterComponent.h>
#include <Engine/ECS/Components/SmokeEmitterComponent.h>

int main() {
    using namespace Engine;
    using namespace Engine::Particles;

    ParticleEmitter emitter;
    if (emitter.position.x() != 0.0F || emitter.minVelocity.y() != 1.0F ||
        emitter.maxVelocity.y() != 4.0F || emitter.minLifeTime != 1.0F ||
        emitter.maxLifeTime != 2.0F || emitter.minSize != 0.04F ||
        emitter.maxSize != 0.12F || emitter.spawnRate != 200.0F ||
        emitter.accumulator != 0.0F) return 1;

    SmokeEmitter smoke;
    if (smoke.minVelocity.x() != -0.24F || smoke.maxVelocity.y() != 1.05F ||
        smoke.color.a() != 0.19F || smoke.minLifeTime != 5.5F ||
        smoke.maxLifeTime != 9.0F || smoke.minSize != 0.28F ||
        smoke.maxSize != 0.88F || smoke.spawnRate != 260.0F ||
        smoke.buoyancy != 2.25F || smoke.drag != 0.68F ||
        smoke.turbulence != 0.30F || smoke.collisionRadius != 0.10F) return 2;

    ParticleEmitterComponent particleComponent;
    SmokeEmitterComponent smokeComponent;
    if (particleComponent.emitter.spawnRate != 200.0F ||
        smokeComponent.emitter.spawnRate != 260.0F ||
        smokeComponent.emitter.buoyancy != 2.25F) return 3;

    ColorPickerComponent picker;
    if (picker.color.r() != 1.0F || picker.color.g() != 1.0F ||
        picker.color.b() != 1.0F || picker.color.a() != 1.0F) return 4;
    picker.color = Color::from_rgba(0.1F, 0.2F, 0.3F, 0.4F);
    if (picker.color.r() != 0.1F || picker.color.a() != 0.4F) return 5;
    return 0;
}