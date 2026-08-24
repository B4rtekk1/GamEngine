#include <Engine/ECS/Components/ColliderComponent.h>
#include <Engine/ECS/Components/RigidbodyComponent.h>
#include <Engine/Scene/Components/LightComponent.h>

#include <variant>

int main() {
    using namespace Engine;

    RigidbodyComponent body;
    if (body.type != RigidbodyType::Dynamic || body.mass != 1.0f ||
        body.linearDamping != 0.05f || body.angularDamping != 0.05f ||
        !body.useGravity || body.fixedRotation || body.runtimeBody != 0) return 1;
    body.type = RigidbodyType::Kinematic;
    body.mass = 2.5f;
    body.linearVelocity = {1.0f, 2.0f, 3.0f};
    if (body.type != RigidbodyType::Kinematic || body.mass != 2.5f ||
        body.linearVelocity.z() != 3.0f) return 2;

    ColliderComponent collider;
    if (!std::holds_alternative<BoxCollider>(collider.shape) ||
        collider.offset.x() != 0.0f || collider.isTrigger || collider.friction != 0.5f ||
        collider.restitution != 0.0f) return 3;
    collider.shape = SphereCollider{2.0f};
    if (!std::holds_alternative<SphereCollider>(collider.shape) ||
        std::get<SphereCollider>(collider.shape).radius != 2.0f) return 4;
    collider.shape = CapsuleCollider{0.4f, 1.8f};
    if (!std::holds_alternative<CapsuleCollider>(collider.shape) ||
        std::get<CapsuleCollider>(collider.shape).height != 1.8f) return 5;
    collider.shape = RampCollider{{3.0f, 2.0f, 2.0f}};
    collider.isTrigger = true;
    collider.friction = 0.2f;
    collider.restitution = 0.8f;
    if (!std::holds_alternative<RampCollider>(collider.shape) || !collider.isTrigger ||
        collider.friction != 0.2f || collider.restitution != 0.8f) return 6;

    LightComponent light;
    if (light.type != LightType::Directional || light.color.r() != 1.0f ||
        light.color.g() != 1.0f || light.color.b() != 1.0f || light.intensity != 1.0f ||
        !light.enabled || !light.castShadows) return 7;
    light.type = LightType::Spot;
    light.color = {0.3f, 0.4f, 0.5f};
    light.intensity = 8.0f;
    light.enabled = false;
    light.castShadows = false;
    if (light.type != LightType::Spot || light.color.b() != 0.5f ||
        light.intensity != 8.0f || light.enabled || light.castShadows) return 8;
    return 0;
}
