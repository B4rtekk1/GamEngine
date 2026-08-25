#include <Engine/Physics/PhysicsSystem.h>
#include <Engine/Scene/Scene.h>

#include <cmath>

namespace {
bool near(const float a, const float b, const float epsilon = 0.001F) {
    return std::abs(a - b) <= epsilon;
}
}

int main() {
    using namespace Engine;

    Scene scene;
    Actor noGravity = scene.createActor("NoGravity");
    noGravity.setPosition({0.0F, 10.0F, 0.0F});
    noGravity.addRigidbody(RigidbodyComponent{.useGravity = false});
    noGravity.addBoxCollider();

    Actor staticBody = scene.createActor("Static");
    staticBody.setPosition({3.0F, 2.0F, 0.0F});
    staticBody.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    staticBody.addBoxCollider({2.0F, 0.5F, 2.0F});

    Actor kinematic = scene.createActor("Kinematic");
    kinematic.setPosition({4.0F, 10.0F, 0.0F});
    kinematic.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Kinematic});
    kinematic.addBoxCollider();

    PhysicsSystem physics;
    physics.update(scene, 0.0F);
    physics.update(scene, -1.0F);
    if (!near(noGravity.position().y(), 10.0F) || !near(staticBody.position().y(), 2.0F) ||
        !near(kinematic.position().y(), 10.0F)) return 1;

    const auto invalidDirection = physics.raycast(scene, {0, 0, 0}, {0, 0, 0});
    const auto invalidDistance = physics.raycast(scene, {0, 0, 0}, {0, 1, 0}, 0.0F);
    if (invalidDirection || invalidDistance) return 2;

    Actor far = scene.createActor("Far");
    far.setPosition({0.0F, 20.0F, 0.0F});
    far.addBoxCollider();
    const auto hit = physics.raycast(scene, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, 100.0F);
    if (!hit || hit->actor.name() != "NoGravity" || !near(hit->distance, 9.5F) ||
        !near(hit->point.y(), 9.5F) || !near(hit->normal.y(), -1.0F)) return 3;

    const auto shortHit = physics.raycast(scene, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, 5.0F);
    if (shortHit) return 4;
    const auto sideHit = physics.raycast(scene, {-5.0F, 2.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, 20.0F);
    if (!sideHit || sideHit->actor.name() != "Static" || !near(sideHit->distance, 6.0F) ||
        !near(sideHit->normal.x(), -1.0F)) return 5;

    physics.setGravity({0.0F, -20.0F, 0.0F});
    if (!near(physics.gravity().y(), -20.0F)) return 6;
    return 0;
}