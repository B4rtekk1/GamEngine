#include <gtest/gtest.h>

#include "Engine/Physics/PhysicsSystem.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneEditor.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/ECS/Components/TerrainGrassComponent.h"
#include "Engine/ECS/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/IdentityComponents.h"
#include "Engine/Scene/Components/LightComponent.h"

#include <stdexcept>
#include <filesystem>
#include <fstream>

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

TEST(PhysXPhysics, RaycastRejectsInvalidQueriesAndNormalizesDirection) {
    Engine::Scene scene;
    Engine::PhysicsSystem physics;
    EXPECT_FALSE(physics.raycast(scene, {}, {}, 10.0F).has_value());
    EXPECT_FALSE(physics.raycast(scene, {}, {0.0F, -1.0F, 0.0F}, 0.0F).has_value());
    EXPECT_FALSE(physics.raycast(scene, {}, {0.0F, -1.0F, 0.0F}, -1.0F).has_value());

    const auto target = scene.createActor("Ray target");
    target.setPosition({0.0F, 1.0F, 0.0F});
    target.addBoxCollider({1.0F, 1.0F, 1.0F});
    const auto hit = physics.raycast(scene, {0.0F, 5.0F, 0.0F}, {0.0F, -2.0F, 0.0F}, 10.0F);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->actor.id(), target.id());
    EXPECT_NEAR(hit->distance, 3.0F, 1.0e-4F);

    Engine::Scene emptyScene;
    EXPECT_FALSE(physics.raycast(emptyScene, {}, {1.0F, 0.0F, 0.0F}, 10.0F).has_value());
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

TEST(Scene, KeepsExactlyOneEnabledDirectionalLight) {
    Engine::Scene scene;
    const auto first = scene.createLightActor("Sun A");
    const auto second = scene.createLightActor("Sun B");
    const auto firstEntity = scene.findEntity(first.id());
    const auto secondEntity = scene.findEntity(second.id());
    auto editor = scene.editor();
    EXPECT_FALSE(editor.get<Engine::LightComponent>(firstEntity).enabled);
    EXPECT_TRUE(editor.get<Engine::LightComponent>(secondEntity).enabled);

    scene.setActiveDirectionalLight(firstEntity);
    EXPECT_TRUE(editor.get<Engine::LightComponent>(firstEntity).enabled);
    EXPECT_FALSE(editor.get<Engine::LightComponent>(secondEntity).enabled);

    Engine::LightComponent pointLight;
    pointLight.type = Engine::LightType::Point;
    const auto point = scene.createLightActor("Point", pointLight);
    EXPECT_THROW(scene.setActiveDirectionalLight(scene.findEntity(point.id())), std::invalid_argument);
    EXPECT_THROW(scene.setActiveDirectionalLight(Engine::NullEntity), std::invalid_argument);
}

TEST(Scene, RetainsTheLastPrimaryCameraWhenDestroyed) {
    Engine::Scene scene;
    auto first = scene.createCameraActor("Main camera");
    ASSERT_TRUE(first.valid());
    first.destroy();
    EXPECT_FALSE(first.valid());
    EXPECT_TRUE(scene.findActor("Main camera").valid());

    const auto second = scene.createCameraActor("Backup camera");
    ASSERT_TRUE(second.valid());
    auto retainedFirst = scene.findActor("Main camera");
    retainedFirst.destroy();
    EXPECT_FALSE(retainedFirst.valid());
    EXPECT_TRUE(second.valid());
    EXPECT_FALSE(scene.findActor("Main camera").valid());
}

TEST(SceneSerializer, RoundTripsCubeActorTransformAndMaterial) {
    const auto path = std::filesystem::temp_directory_path() / "gameengine-scene-roundtrip-test.scene";
    std::error_code error;
    std::filesystem::remove(path, error);

    Engine::PBRMaterial material;
    material.metallic = 0.6F;
    material.roughness = 0.3F;
    material.alphaBlend = true;
    Engine::Scene source;
    const auto actor = source.createCube("Serialized cube", material);
    actor.setPosition({1.0F, 2.0F, 3.0F});
    actor.setRotation({10.0F, 20.0F, 30.0F});
    actor.setScale({2.0F, 3.0F, 4.0F});
    ASSERT_NO_THROW(source.save(path));
    ASSERT_TRUE(std::filesystem::exists(path));

    Engine::Scene loaded;
    ASSERT_NO_THROW(loaded.load(path));
    std::filesystem::remove(path, error);
    const auto restored = loaded.findActor("Serialized cube");
    ASSERT_TRUE(restored.valid());
    EXPECT_FLOAT_EQ(restored.position().x(), 1.0F);
    EXPECT_FLOAT_EQ(restored.position().y(), 2.0F);
    EXPECT_FLOAT_EQ(restored.rotation().z(), 30.0F);
    EXPECT_FLOAT_EQ(restored.scale().y(), 3.0F);
    const auto entity = loaded.findEntity(restored.id());
    const auto& renderer = loaded.editor().get<Engine::MeshRendererComponent>(entity);
    ASSERT_TRUE(renderer.hasMesh());
    EXPECT_FLOAT_EQ(renderer.material.metallic, 0.6F);
    EXPECT_FLOAT_EQ(renderer.material.roughness, 0.3F);
    EXPECT_TRUE(renderer.material.alphaBlend);
}

TEST(SceneSerializer, RejectsInvalidFilesWithoutChangingTheScene) {
    const auto path = std::filesystem::temp_directory_path() / "gameengine-invalid-scene-test.scene";
    {
        std::ofstream file{path};
        ASSERT_TRUE(file.is_open());
        file << "this is not a GameEngine scene";
    }

    Engine::Scene scene;
    const auto actor = scene.createActor("Existing actor");
    ASSERT_TRUE(actor.valid());
    EXPECT_THROW(scene.load(path), std::runtime_error);
    std::error_code error;
    std::filesystem::remove(path, error);
    EXPECT_TRUE(scene.findActor("Existing actor").valid());
}

TEST(SceneSerializer, RoundTripsParentRelationshipAndHierarchyOrder) {
    const auto path = std::filesystem::temp_directory_path() / "gameengine-hierarchy-roundtrip-test.scene";
    std::error_code error;
    std::filesystem::remove(path, error);
    Engine::Scene source;
    const auto parent = source.createActor("Parent");
    const auto child = source.createActor("Child");
    const auto parentEntity = source.findEntity(parent.id());
    const auto childEntity = source.findEntity(child.id());
    auto editor = source.editor();
    const auto parentUuid = editor.get<Engine::UUIDComponent>(parentEntity).value;
    editor.add<Engine::ParentComponent>(childEntity, Engine::ParentComponent{parentUuid});
    editor.add<Engine::HierarchyOrderComponent>(childEntity, Engine::HierarchyOrderComponent{7});
    ASSERT_NO_THROW(source.save(path));

    Engine::Scene loaded;
    ASSERT_NO_THROW(loaded.load(path));
    std::filesystem::remove(path, error);
    const auto loadedParent = loaded.findActor("Parent");
    const auto loadedChild = loaded.findActor("Child");
    ASSERT_TRUE(loadedParent.valid());
    ASSERT_TRUE(loadedChild.valid());
    const auto loadedParentEntity = loaded.findEntity(loadedParent.id());
    const auto loadedChildEntity = loaded.findEntity(loadedChild.id());
    auto loadedEditor = loaded.editor();
    EXPECT_EQ(loadedEditor.get<Engine::ParentComponent>(loadedChildEntity).parent,
              loadedEditor.get<Engine::UUIDComponent>(loadedParentEntity).value);
    EXPECT_EQ(loadedEditor.get<Engine::HierarchyOrderComponent>(loadedChildEntity).value, 7u);
}

TEST(SceneSerializer, RejectsCyclicParentRelationships) {
    const auto path = std::filesystem::temp_directory_path() / "gameengine-cyclic-hierarchy-test.scene";
    std::error_code error;
    std::filesystem::remove(path, error);
    Engine::Scene source;
    const auto first = source.createActor("First");
    const auto second = source.createActor("Second");
    const auto firstEntity = source.findEntity(first.id());
    const auto secondEntity = source.findEntity(second.id());
    auto editor = source.editor();
    const auto firstUuid = editor.get<Engine::UUIDComponent>(firstEntity).value;
    const auto secondUuid = editor.get<Engine::UUIDComponent>(secondEntity).value;
    editor.add<Engine::ParentComponent>(firstEntity, Engine::ParentComponent{secondUuid});
    editor.add<Engine::ParentComponent>(secondEntity, Engine::ParentComponent{firstUuid});
    ASSERT_NO_THROW(source.save(path));

    Engine::Scene target;
    static_cast<void>(target.createActor("Existing"));
    EXPECT_THROW(target.load(path), std::runtime_error);
    std::filesystem::remove(path, error);
    EXPECT_TRUE(target.findActor("Existing").valid());
}

TEST(Actor, UpdatesPhysicsAndCameraComponentsThroughHighLevelApi) {
    Engine::Scene scene;
    const auto actor = scene.createActor("Configurable actor");
    actor.setPosition({1.0F, 2.0F, 3.0F});
    actor.translate({-1.0F, 1.0F, 2.0F});
    actor.setScale({2.0F, 3.0F, 4.0F});
    EXPECT_FLOAT_EQ(actor.position().x(), 0.0F);
    EXPECT_FLOAT_EQ(actor.position().y(), 3.0F);
    EXPECT_FLOAT_EQ(actor.scale().z(), 4.0F);

    actor.addRigidbody();
    actor.setMass(4.0F);
    actor.setGravityEnabled(false);
    actor.setVelocity({5.0F, 0.0F, -1.0F});
    actor.addSphereCollider(2.0F);
    actor.setColliderTrigger(true);
    actor.setColliderMaterial(0.2F, 0.8F);
    actor.setOrthographicCamera(25.0F, 0.5F, 200.0F);
    ASSERT_TRUE(actor.hasRigidbody());
    ASSERT_TRUE(actor.hasCollider());
    ASSERT_TRUE(actor.hasCamera());

    const auto entity = scene.findEntity(actor.id());
    auto editor = scene.editor();
    const auto& body = editor.get<Engine::RigidbodyComponent>(entity);
    EXPECT_FLOAT_EQ(body.mass, 4.0F);
    EXPECT_FALSE(body.useGravity);
    EXPECT_FLOAT_EQ(body.linearVelocity.z(), -1.0F);
    const auto& collider = editor.get<Engine::ColliderComponent>(entity);
    ASSERT_TRUE(std::holds_alternative<Engine::SphereCollider>(collider.shape));
    EXPECT_FLOAT_EQ(std::get<Engine::SphereCollider>(collider.shape).radius, 2.0F);
    EXPECT_TRUE(collider.isTrigger);
    EXPECT_FLOAT_EQ(collider.friction, 0.2F);
    EXPECT_FLOAT_EQ(collider.restitution, 0.8F);
    const auto& camera = editor.get<Engine::CameraComponent>(entity);
    EXPECT_TRUE(camera.isOrthographic());
    EXPECT_FLOAT_EQ(camera.orthographicSize, 25.0F);
    EXPECT_FLOAT_EQ(camera.nearClip, 0.5F);
    EXPECT_FLOAT_EQ(camera.farClip, 200.0F);
}

} // namespace
