#include <gtest/gtest.h>

#include "Engine/Physics/PhysicsSystem.h"
#include "Engine/Scene/Scene.h"

namespace {

TEST(PhysXPhysics, DynamicBoxFallsAndRestsOnStaticGround) {
    Engine::Scene scene;
    const Engine::Actor ground = scene.createActor("Ground");
    ground.addBoxCollider({5.0F, 0.5F, 5.0F});

    const Engine::Actor box = scene.createActor("Box");
    box.setPosition({0.0F, 3.0F, 0.0F});
    box.addBoxCollider({0.5F, 0.5F, 0.5F});
    box.addRigidbody();

    Engine::PhysicsSystem physics;
    for (int step = 0; step < 240; ++step) physics.update(scene, 1.0F / 60.0F);

    EXPECT_NEAR(box.position().y(), 1.0F, 0.03F);
    EXPECT_LT(box.velocity().length(), 0.05F);
}

TEST(PhysXPhysics, TriggerDoesNotStopDynamicBody) {
    Engine::Scene scene;
    const Engine::Actor trigger = scene.createActor("Trigger");
    trigger.addBoxCollider({5.0F, 0.25F, 5.0F});
    trigger.setColliderTrigger(true);

    const Engine::Actor sphere = scene.createActor("Sphere");
    sphere.setPosition({0.0F, 2.0F, 0.0F});
    sphere.addSphereCollider(0.5F);
    sphere.addRigidbody();

    Engine::PhysicsSystem physics;
    for (int step = 0; step < 90; ++step) physics.update(scene, 1.0F / 60.0F);

    EXPECT_LT(sphere.position().y(), -2.0F);
}

TEST(PhysXPhysics, RaycastReturnsPhysXHitDataAndActor) {
    Engine::Scene scene;
    const Engine::Actor target = scene.createActor("Ray target");
    target.setPosition({0.0F, 1.0F, 0.0F});
    target.addBoxCollider({1.0F, 1.0F, 1.0F});

    Engine::PhysicsSystem physics;
    const auto hit = physics.raycast(
        scene, {0.0F, 5.0F, 0.0F}, {0.0F, -1.0F, 0.0F}, 10.0F);

    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->actor.name(), "Ray target");
    EXPECT_NEAR(hit->distance, 3.0F, 1.0e-4F);
    EXPECT_NEAR(hit->point.y(), 2.0F, 1.0e-4F);
    EXPECT_NEAR(hit->normal.y(), 1.0F, 1.0e-4F);
}

} // namespace
