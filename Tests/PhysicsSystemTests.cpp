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

    auto sphere = scene.createActor("Rolling sphere");
    sphere.setPosition({0.0f, 0.55f, 0.0f});
    sphere.addRigidbody(RigidbodyComponent{.useGravity = false, .linearVelocity = {1.0f, 0.0f, 0.0f}});
    sphere.addSphereCollider(0.5f);

    physics.update(scene, 1.0f);
    if (std::abs(sphere.position().x() - 1.0f) > 0.001f ||
        std::abs(sphere.rotation().z() + 114.59156f) > 0.001f) {
        return 2;
    }

    Scene slopeScene;
    auto ramp = slopeScene.createActor("Ramp");
    ramp.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    ramp.addRampCollider({3.0f, 2.0f, 2.0f});
    auto slopeSphere = slopeScene.createActor("Slope sphere");
    slopeSphere.setPosition({0.0f, 0.7071068f, 0.0f});
    slopeSphere.addRigidbody();
    slopeSphere.addSphereCollider(0.5f);

    physics.update(slopeScene, 0.1f);
    if (slopeSphere.velocity().z() >= -0.01f || slopeSphere.rotation().x() >= -0.01f) {
        return 3;
    }

    Scene slipperySlopeScene;
    auto slipperyRamp = slipperySlopeScene.createActor("Slippery ramp");
    slipperyRamp.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    slipperyRamp.addRampCollider({3.0f, 2.0f, 2.0f});
    slipperyRamp.setColliderMaterial(0.0f, 0.0f);
    auto slidingSphere = slipperySlopeScene.createActor("Sliding sphere");
    slidingSphere.setPosition({0.0f, 0.7071068f, 0.0f});
    slidingSphere.addRigidbody();
    slidingSphere.addSphereCollider(0.5f);
    slidingSphere.setColliderMaterial(0.0f, 0.0f);

    physics.update(slipperySlopeScene, 0.1f);
    if (slidingSphere.velocity().z() >= -0.45f ||
        std::abs(slidingSphere.rotation().x()) > 0.001f) {
        return 4;
    }

    Scene connectedRampsScene;
    auto firstRamp = connectedRampsScene.createActor("First ramp");
    firstRamp.setPosition({0.0f, 2.0f, 0.0f});
    firstRamp.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    firstRamp.addRampCollider({3.0f, 2.0f, 2.0f});
    auto secondRamp = connectedRampsScene.createActor("Second ramp");
    secondRamp.setPosition({0.0f, -2.0f, -4.0f});
    secondRamp.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    secondRamp.addRampCollider({3.0f, 2.0f, 2.0f});
    auto connectedSphere = connectedRampsScene.createActor("Connected-ramp sphere");
    connectedSphere.setPosition({0.0f, 2.0f + 1.5f + 0.7071068f, 1.5f});
    connectedSphere.addRigidbody();
    connectedSphere.addSphereCollider(0.5f);

    for (int step = 0; step < 16; ++step) physics.update(connectedRampsScene, 0.1f);
    const float expectedSecondRampY = -2.0f + (connectedSphere.position().z() + 4.0f) + 0.7071068f;
    if (connectedSphere.position().z() >= -2.5f || connectedSphere.position().z() <= -4.0f ||
        std::abs(connectedSphere.position().y() - expectedSecondRampY) > 0.01f) {
        return 5;
    }

    Scene rampEntryScene;
    auto entryGround = rampEntryScene.createActor("Ground");
    entryGround.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    entryGround.addBoxCollider({10.0f, 0.05f, 10.0f});
    auto entryRamp = rampEntryScene.createActor("Ramp");
    entryRamp.setPosition({0.0f, 2.05f, 0.0f});
    entryRamp.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    entryRamp.addRampCollider({3.0f, 2.0f, 2.0f});
    auto enteringSphere = rampEntryScene.createActor("Entering sphere");
    enteringSphere.setPosition({0.0f, 0.55f, -2.5f});
    enteringSphere.addRigidbody(RigidbodyComponent{
        .linearVelocity = {0.0f, 0.0f, 4.0f}
    });
    enteringSphere.addSphereCollider(0.5f);

    for (int step = 0; step < 40; ++step) physics.update(rampEntryScene, 0.01f);
    if (enteringSphere.position().z() <= -1.9f ||
        enteringSphere.position().y() <= 0.65f ||
        enteringSphere.velocity().y() <= 0.0f) {
        return 6;
    }

    Scene rampExitScene;
    auto exitGround = rampExitScene.createActor("Ground");
    exitGround.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    exitGround.addBoxCollider({10.0f, 0.05f, 10.0f});
    auto exitRamp = rampExitScene.createActor("Ramp");
    exitRamp.setPosition({0.0f, 2.05f, 0.0f});
    exitRamp.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    exitRamp.addRampCollider({3.0f, 2.0f, 2.0f});
    auto exitingSphere = rampExitScene.createActor("Exiting sphere");
    exitingSphere.setPosition({0.0f, 1.05f + 0.7071068f, -1.0f});
    exitingSphere.addRigidbody();
    exitingSphere.addSphereCollider(0.5f);

    for (int step = 0; step < 120; ++step) physics.update(rampExitScene, 0.01f);
    if (exitingSphere.position().z() >= -2.5f ||
        exitingSphere.position().y() < 0.549f) {
        return 7;
    }

    Scene rollingResistanceScene;
    auto resistanceGround = rollingResistanceScene.createActor("Ground");
    resistanceGround.addRigidbody(RigidbodyComponent{.type = RigidbodyType::Static});
    resistanceGround.addBoxCollider({20.0f, 0.05f, 20.0f});
    auto slowingSphere = rollingResistanceScene.createActor("Slowing sphere");
    slowingSphere.setPosition({0.0f, 0.55f, 0.0f});
    slowingSphere.addRigidbody(RigidbodyComponent{
        .linearVelocity = {2.0f, 0.0f, 0.0f}
    });
    slowingSphere.addSphereCollider(0.5f);

    physics.update(rollingResistanceScene, 0.1f);
    const float speedAfterFirstStep = slowingSphere.velocity().x();
    for (int step = 0; step < 100; ++step) physics.update(rollingResistanceScene, 0.1f);
    if (speedAfterFirstStep >= 2.0f || slowingSphere.velocity().length() > 0.001f ||
        slowingSphere.position().y() < 0.549f) {
        return 8;
    }

    return 0;
}
