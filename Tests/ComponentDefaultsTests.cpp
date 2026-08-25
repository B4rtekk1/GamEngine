#include <Engine/ECS/Components/ColliderComponent.h>
#include <Engine/ECS/Components/RigidbodyComponent.h>
#include <Engine/Scene/Components/LightComponent.h>

#include <variant>

int main() {
    using namespace Engine;

    RigidbodyComponent body;
    if (body.type != RigidbodyType::Dynamic || body.mass != 1.0F ||
        body.linearDamping != 0.05F || body.angularDamping != 0.05F ||
        !body.useGravity || body.fixedRotation || body.runtimeBody != 0) return 1;
    body.type = RigidbodyType::Kinematic;
    body.mass = 2.5F;
    body.linearVelocity = {1.0F, 2.0F, 3.0F};
    if (body.type != RigidbodyType::Kinematic || body.mass != 2.5F ||
        body.linearVelocity.z() != 3.0F) return 2;

    ColliderComponent collider;
    if (!std::holds_alternative<BoxCollider>(collider.shape) ||
        collider.offset.x() != 0.0F || collider.isTrigger || collider.friction != 0.5F ||
        collider.restitution != 0.0F) return 3;
    collider.shape = SphereCollider{2.0F};
    if (!std::holds_alternative<SphereCollider>(collider.shape) ||
        std::get<SphereCollider>(collider.shape).radius != 2.0F) return 4;
    collider.shape = CapsuleCollider{0.4F, 1.8F};
    if (!std::holds_alternative<CapsuleCollider>(collider.shape) ||
        std::get<CapsuleCollider>(collider.shape).height != 1.8F) return 5;
    collider.shape = RampCollider{{3.0F, 2.0F, 2.0F}};
    collider.isTrigger = true;
    collider.friction = 0.2F;
    collider.restitution = 0.8F;
    if (!std::holds_alternative<RampCollider>(collider.shape) || !collider.isTrigger ||
        collider.friction != 0.2F || collider.restitution != 0.8F) return 6;

    LightComponent light;
    if (light.type != LightType::Directional || light.color.r() != 1.0F ||
        light.color.g() != 1.0F || light.color.b() != 1.0F || light.intensity != 1.0F ||
        !light.enabled || !light.castShadows) return 7;
    light.type = LightType::Spot;
    light.color = {0.3F, 0.4F, 0.5F};
    light.intensity = 8.0F;
    light.enabled = false;
    light.castShadows = false;
    if (light.type != LightType::Spot || light.color.b() != 0.5F ||
        light.intensity != 8.0F || light.enabled || light.castShadows) return 8;
    return 0;
}