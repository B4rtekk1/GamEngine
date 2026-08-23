#include <Engine/Core/Transform.h>
#include <Engine/ECS/Components/RigidbodyComponent.h>
#include <Engine/ECS/Components/ColliderComponent.h>
#include <Engine/ECS/Registry.h>
#include <Engine/Physics/PhysicsSystem.h>

#include <cmath>

int main() {
    using namespace Engine;

    Registry registry;
    const Entity dynamic = registry.create();
    registry.add<Transform>(dynamic, Transform{.position = {0.0f, 10.0f, 0.0f}});
    registry.add<RigidbodyComponent>(dynamic);
    registry.add<ColliderComponent>(dynamic);

    const Entity staticBody = registry.create();
    registry.add<Transform>(staticBody, Transform{.position = {0.0f, 0.0f, 0.0f}});
    registry.add<RigidbodyComponent>(staticBody, RigidbodyComponent{.type = RigidbodyType::Static});
    registry.add<ColliderComponent>(staticBody, ColliderComponent{
        .shape = BoxCollider{.halfExtents = {10.0f, 0.05f, 10.0f}},
        .offset = {0.0f, -0.05f, 0.0f}});

    PhysicsSystem physics;
    physics.update(registry, 1.0f);

    const auto& body = registry.get<RigidbodyComponent>(dynamic);
    const auto& transform = registry.get<Transform>(dynamic);
    const auto& staticTransform = registry.get<Transform>(staticBody);
    if (std::abs(body.linearVelocity.y()) > 0.001f ||
        std::abs(transform.position.y() - 0.5f) > 0.001f ||
        staticTransform.position.y() != 0.0f) {
        return 1;
    }

    return 0;
}
