#include <Engine/Physics/PhysicsSystem.h>
#include <Engine/Scene/Scene.h>

#include <cmath>

namespace {
bool near(const float a, const float b, const float epsilon = 0.001f) {
    return std::abs(a - b) <= epsilon;
}
}

int main() {
    using namespace Engine;

    Scene scene;
    Actor noGravity = scene.createActor("NoGravity");
    noGravity.setPosition({0.0f, 10.0f, 0.0f});
    noGravity.addRigidbody(RigidbodyComponent{.useGravity = false});
    noGravity.addBoxCollider();

    Actor staticBody = scene.createActor("Static");
    staticBody.setPosition({3.0f, 2.0f, 0.0f});
    staticBody.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    staticBody.addBoxCollider({2.0f, 0.5f, 2.0f});

    Actor kinematic = scene.createActor("Kinematic");
    kinematic.setPosition({4.0f, 10.0f, 0.0f});
    kinematic.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Kinematic});
    kinematic.addBoxCollider();

    PhysicsSystem physics;
    physics.update(scene, 0.0f);
    physics.update(scene, -1.0f);
    if (!near(noGravity.position().y(), 10.0f) || !near(staticBody.position().y(), 2.0f) ||
        !near(kinematic.position().y(), 10.0f)) return 1;

    const auto invalidDirection = physics.raycast(scene, {0, 0, 0}, {0, 0, 0});
    const auto invalidDistance = physics.raycast(scene, {0, 0, 0}, {0, 1, 0}, 0.0f);
    if (invalidDirection || invalidDistance) return 2;

    Actor far = scene.createActor("Far");
    far.setPosition({0.0f, 20.0f, 0.0f});
    far.addBoxCollider();
    const auto hit = physics.raycast(scene, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 100.0f);
    if (!hit || hit->actor.name() != "NoGravity" || !near(hit->distance, 9.5f) ||
        !near(hit->point.y(), 9.5f) || !near(hit->normal.y(), -1.0f)) return 3;

    const auto shortHit = physics.raycast(scene, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 5.0f);
    if (shortHit) return 4;
    const auto sideHit = physics.raycast(scene, {-5.0f, 2.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 20.0f);
    if (!sideHit || sideHit->actor.name() != "Static" || !near(sideHit->distance, 6.0f) ||
        !near(sideHit->normal.x(), -1.0f)) return 5;

    physics.setGravity({0.0f, -20.0f, 0.0f});
    if (!near(physics.gravity().y(), -20.0f)) return 6;
    return 0;
}
