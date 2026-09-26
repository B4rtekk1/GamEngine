#include <gtest/gtest.h>

#include "Engine/Physics/PhysicsSystem.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneEditor.h"
#include "Engine/ECS/Components/TerrainGrassComponent.h"

namespace {

TEST(PhysXPhysics, CachedStaticColliderFollowsParentPoseScaleAndReplacement) {
    Engine::Scene scene;
    const auto parent = scene.createActor("Parent");
    const auto child = scene.createActor("Collider");
    child.addBoxCollider({1.0F, 1.0F, 1.0F});
    child.setParent(parent, Engine::ParentMode::KeepLocal);
    const auto cast = [&](const float x) {
        return scene.physics().raycast({x, 10.0F, 0.0F}, {0.0F, -1.0F, 0.0F}, 20.0F);
    };
    ASSERT_TRUE(cast(0.0F));
    ASSERT_TRUE(cast(0.0F)); // Exercise the unchanged revision path.
    parent.setPosition({4.0F, 2.0F, 0.0F});
    EXPECT_FALSE(cast(0.0F));
    const auto moved = cast(4.0F);
    ASSERT_TRUE(moved);
    EXPECT_NEAR(moved->distance, 7.0F, 1.0e-4F);
    parent.setScale({2.0F, 2.0F, 2.0F});
    const auto scaled = cast(5.5F);
    ASSERT_TRUE(scaled);
    EXPECT_NEAR(scaled->distance, 6.0F, 1.0e-4F);
    child.clearParent(Engine::ParentMode::KeepWorld);
    ASSERT_TRUE(cast(5.5F));
    const auto entity = scene.findEntity(child.id());
    scene.editor().remove<Engine::ColliderComponent>(entity);
    EXPECT_FALSE(cast(4.0F));
    child.addSphereCollider(0.5F);
    const auto replaced = cast(4.0F);
    ASSERT_TRUE(replaced);
    EXPECT_NEAR(replaced->distance, 7.0F, 1.0e-4F);
}


TEST(PlayModePhysics, ReplacingTransformDoesNotReuseStalePhysicsPose) {
    Engine::Scene scene;
    const auto actor = scene.createActor("Static");
    actor.addBoxCollider({1.0F, 1.0F, 1.0F});
    const auto cast = [&] {
        return scene.physics().raycast({0.0F, 10.0F, 0.0F}, {0.0F, -1.0F, 0.0F}, 20.0F);
    };
    ASSERT_TRUE(cast());
    const auto entity = scene.findEntity(actor.id());
    scene.editor().remove<Engine::Transform>(entity);
    scene.editor().add<Engine::Transform>(entity, Engine::Transform{.position = {0.0F, 3.0F, 0.0F}});
    const auto moved = cast();
    ASSERT_TRUE(moved);
    EXPECT_NEAR(moved->distance, 6.0F, 1.0e-4F);
}

TEST(PlayModePhysics, DynamicCommandsStillApplyWithoutWorldTrsDecomposition) {
    Engine::Scene scene;
    const auto actor = scene.createActor("Dynamic");
    actor.addSphereCollider(0.5F);
    actor.addRigidbody();
    Engine::PhysicsSystem physics{{0.0F, 0.0F, 0.0F}};
    physics.update(scene, 1.0F / 60.0F);
    actor.setVelocity({6.0F, 0.0F, 0.0F});
    for (int step = 0; step < 10; ++step) physics.update(scene, 1.0F / 60.0F);
    EXPECT_GT(actor.position().x(), 0.8F);
    const auto entity = scene.findEntity(actor.id());
    EXPECT_FALSE(scene.editor().read<Engine::Transform>(entity).worldTrsValid);
    actor.teleport({10.0F, 3.0F, 0.0F});
    physics.update(scene, 1.0F / 60.0F);
    EXPECT_GT(actor.position().x(), 10.0F);
    EXPECT_NEAR(actor.position().y(), 3.0F, 1.0e-4F);
}

TEST(PlayModePhysics, KinematicPoseAndScaleChangesReachSceneQueries) {
    Engine::Scene scene;
    const auto actor = scene.createActor("Kinematic");
    actor.addBoxCollider({1.0F, 1.0F, 1.0F});
    actor.addRigidbody(Engine::RigidbodyComponent{.type = Engine::RigidbodyType::Kinematic});
    Engine::PhysicsSystem physics;
    physics.update(scene, 1.0F / 60.0F);
    actor.setPosition({0.0F, 3.0F, 0.0F});
    physics.update(scene, 1.0F / 60.0F);
    physics.update(scene, 1.0F / 60.0F);
    const auto hit = physics.raycast(scene, {0.0F, 10.0F, 0.0F}, {0.0F, -1.0F, 0.0F}, 20.0F);
    ASSERT_TRUE(hit);
    EXPECT_NEAR(hit->distance, 6.0F, 1.0e-4F);
    actor.setScale({2.0F, 2.0F, 2.0F});
    physics.update(scene, 1.0F / 60.0F);
    const auto scaled = physics.raycast(scene, {1.5F, 10.0F, 0.0F}, {0.0F, -1.0F, 0.0F}, 20.0F);
    ASSERT_TRUE(scaled);
    EXPECT_NEAR(scaled->distance, 5.0F, 1.0e-4F);
}

TEST(PlayModePhysics, GrassAddedAfterEmptySceneStillTramplesAndRecovers) {
    Engine::Scene scene;
    const auto actor = scene.createActor("Trampler");
    actor.setPosition({0.0F, 0.5F, 0.0F});
    actor.addSphereCollider(0.5F);
    Engine::PhysicsSystem physics;
    physics.update(scene, 1.0F / 60.0F);

    const auto terrain = scene.createActor("Grass");
    const auto entity = scene.findEntity(terrain.id());
    Engine::TerrainGrassComponent grass;
    grass.instances.push_back({.position = {0.0F, 0.0F, 0.0F}});
    scene.editor().add<Engine::TerrainGrassComponent>(entity, std::move(grass));
    physics.update(scene, 1.0F / 60.0F);
    const float trampled = scene.editor().read<Engine::TerrainGrassComponent>(entity).instances.front().trampled;
    EXPECT_GT(trampled, 0.5F);
    actor.setPosition({10.0F, 0.5F, 0.0F});
    physics.update(scene, 1.0F / 60.0F);
    EXPECT_LT(scene.editor().read<Engine::TerrainGrassComponent>(entity).instances.front().trampled, trampled);
    scene.editor().remove<Engine::TerrainGrassComponent>(entity);
    EXPECT_NO_THROW(physics.update(scene, 1.0F / 60.0F));
}

} // namespace
