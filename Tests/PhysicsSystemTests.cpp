#include <Engine/Scene/Scene.h>
#include <Engine/Physics/PhysicsSystem.h>

#include <cmath>

int main() {
    using namespace Engine;

    Scene scene;
    auto dynamic = scene.createActor("Dynamic");
    dynamic.setPosition({0.0f, 10.0f, 0.0f});
    dynamic.addRigidbody();
    dynamic.addBoxCollider({0.5f, 0.5f, 0.5f});

    auto staticBody = scene.createActor("Ground");
    staticBody.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    staticBody.addBoxCollider({10.0f, 0.05f, 10.0f});

    PhysicsSystem physics;
    physics.update(scene, 1.0f);

    if (std::abs(dynamic.position().y() - 0.55f) > 0.001f ||
        staticBody.position().y() != 0.0f) {
        return 1;
    }

    return 0;
}
