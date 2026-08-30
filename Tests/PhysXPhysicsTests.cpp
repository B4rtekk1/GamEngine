#include <gtest/gtest.h>

#include "Engine/Physics/PhysicsSystem.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneEditor.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/ECS/Components/TerrainGrassComponent.h"
#include "Engine/ECS/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/IdentityComponents.h"

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

TEST(PhysXPhysics, DynamicSphereRestsOnTerrainMesh) {
    Engine::Scene scene;
    static_cast<void>(scene.createTerrain(
        "Terrain", Engine::TerrainComponent{33, 100.0F, 100.0F, -10.0F, 10.0F}));

    const Engine::Actor sphere = scene.createActor("Sphere");
    sphere.setPosition({0.0F, 3.0F, 0.0F});
    sphere.addSphereCollider(0.5F);
    sphere.addRigidbody();

    Engine::PhysicsSystem physics;
    for (int step = 0; step < 240; ++step) physics.update(scene, 1.0F / 60.0F);

    EXPECT_NEAR(sphere.position().y(), 0.5F, 0.03F);
    EXPECT_LT(sphere.velocity().length(), 0.05F);
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

TEST(PhysXPhysics, SpherePermanentlyTramplesNearbyTerrainGrass) {
    Engine::Scene scene;
    const Engine::Actor terrain = scene.createTerrain(
        "Terrain", Engine::TerrainComponent{5, 4.0F, 4.0F, -1.0F, 1.0F});
    Engine::TerrainGrassComponent grass;
    grass.instances.push_back({.position = {0.0F, 0.0F, 0.0F}});
    scene.editor().add<Engine::TerrainGrassComponent>(scene.findEntity(terrain.id()), grass);

    const Engine::Actor sphere = scene.createActor("Trampling sphere");
    sphere.setPosition({0.0F, 0.5F, 0.0F});
    sphere.addSphereCollider(0.5F);
    sphere.addRigidbody();
    sphere.setVelocity({5.0F, 0.0F, 0.0F});

    Engine::PhysicsSystem physics;
    physics.update(scene, 1.0F / 60.0F);
    const auto entity = scene.findEntity(terrain.id());
    const float trampled = scene.editor().get<Engine::TerrainGrassComponent>(entity)
                                .instances.front().trampled;
    EXPECT_GT(trampled, 0.5F);
    EXPECT_LT(sphere.velocity().x(), 5.0F);

    sphere.setPosition({10.0F, 0.5F, 0.0F});
    physics.update(scene, 1.0F / 60.0F);
    EXPECT_FLOAT_EQ(scene.editor().get<Engine::TerrainGrassComponent>(entity)
                        .instances.front().trampled, trampled);
}

TEST(Prefab, CubePreservesRenderSettingsWhenInstantiatedInScene) {
    Engine::PBRMaterial material;
    material.metallic = 0.7F;
    material.roughness = 0.2F;
    auto prefab = Engine::Prefab::cube(material);
    prefab.setCastShadow(false);
    prefab.setCullingBatch(9);
    ASSERT_NE(prefab.mesh(), nullptr);
    EXPECT_EQ(prefab.mesh()->vertices.size(), 24u);
    EXPECT_EQ(prefab.mesh()->indices.size(), 36u);

    Engine::Scene scene;
    const auto actor = scene.createPrefab("Prefab cube", prefab);
    ASSERT_TRUE(actor.valid());
    EXPECT_EQ(actor.name(), "Prefab cube");
    const auto entity = scene.findEntity(actor.id());
    ASSERT_NE(entity, Engine::NullEntity);
    const auto& renderer = scene.editor().get<Engine::MeshRendererComponent>(entity);
    EXPECT_EQ(renderer.mesh, prefab.mesh());
    EXPECT_FLOAT_EQ(renderer.material.metallic, 0.7F);
    EXPECT_FLOAT_EQ(renderer.material.roughness, 0.2F);
    EXPECT_FALSE(renderer.castShadow);
    EXPECT_EQ(renderer.cullingBatch, 9u);
}

TEST(Scene, MaintainsUniqueNamesAndFreshIdentityAcrossDuplication) {
    Engine::Scene scene;
    const auto first = scene.createActor("Player");
    auto second = scene.createActor("Player");
    ASSERT_TRUE(first.valid());
    ASSERT_TRUE(second.valid());
    EXPECT_EQ(first.name(), "Player");
    EXPECT_EQ(second.name(), "Player 2");

    second.setName("Enemy");
    EXPECT_EQ(second.name(), "Enemy");
    EXPECT_TRUE(scene.findActor("Player").valid());
    EXPECT_TRUE(scene.findActor("Enemy").valid());

    const auto firstEntity = scene.findEntity(first.id());
    const auto originalUuid = scene.editor().get<Engine::UUIDComponent>(firstEntity).value;
    const auto copy = scene.duplicate(first);
    ASSERT_TRUE(copy.valid());
    EXPECT_EQ(copy.name(), "Player 2");
    const auto copyEntity = scene.findEntity(copy.id());
    EXPECT_NE(scene.editor().get<Engine::UUIDComponent>(copyEntity).value, originalUuid);

    second.destroy();
    EXPECT_FALSE(second.valid());
    EXPECT_FALSE(scene.findActor("Enemy").valid());
    EXPECT_TRUE(first.valid());
    EXPECT_TRUE(copy.valid());
}

} // namespace
