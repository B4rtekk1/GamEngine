#include <gtest/gtest.h>
#include "Engine/Physics/GlassDestructionSystem.h"
#include "Engine/Physics/PhysicsSystem.h"
#include "Engine/Scene/SceneEditor.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/ECS/Components/RigidbodyRuntime.h"
#include <sstream>
#include <algorithm>
#include <limits>

namespace {
    std::size_t attachedCount(const Engine::GlassDestructionComponent &glass) {
        return std::ranges::count_if(glass.fragments, [](const auto &fragment) { return fragment.attached; });
    }

    TEST(GlassDestruction, LegacyGlassCubeLoadedFromSceneIsDestructibleOnFirstShot) {
        Engine::Scene authored;
        Engine::PBRMaterial material;
        material.transmission = 0.779F;
        material.thickness = 0.01F;
        const auto pane = authored.createCube("Glass", material);
        pane.setPosition({0.0F, 1.0F, -1.9107282F});
        pane.setScale({1.0F, 2.0F, 0.01F});
        pane.addBoxCollider();
        std::ostringstream saved;
        Engine::SceneSerializer::save(authored, saved);
        Engine::Scene loaded;
        std::istringstream input(saved.str());
        Engine::SceneSerializer::load(loaded, input);
        const auto actor = loaded.findActor("Glass");
        const auto entity = loaded.findEntity(actor.id());
        Engine::SceneEditor editor(loaded);
        const bool hadDestruction = editor.has<Engine::GlassDestructionComponent>(entity);
        EXPECT_FALSE(hadDestruction);
        ASSERT_TRUE(loaded.physics().shootRay({0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, -1.0F}));
        ASSERT_TRUE(editor.has<Engine::GlassDestructionComponent>(entity));
        const auto &glass = editor.read<Engine::GlassDestructionComponent>(entity);
        EXPECT_TRUE(glass.fractured);
        EXPECT_EQ(glass.size, glm::vec2(1.0F));
        EXPECT_FLOAT_EQ(glass.thickness, 1.0F);
        EXPECT_LT(attachedCount(glass), glass.fragments.size());
        // Expire debris so the second ray checks only the remaining pane.
        Engine::GlassDestructionSystem::update(loaded, 5.0F);
        EXPECT_FALSE(loaded.physics().raycast({0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, -1.0F}));
    }

    TEST(GlassDestruction, LegacyRegistrationPreservesOpaqueThickAndDynamicObjects) {
        Engine::Scene scene;
        Engine::PBRMaterial glassMaterial;
        glassMaterial.transmission = 0.9F;
        const auto opaque = scene.createCube("Opaque");
        opaque.setScale({1.0F, 2.0F, 0.01F});
        opaque.addBoxCollider();
        const auto thick = scene.createCube("Thick", glassMaterial);
        thick.addBoxCollider();
        const auto dynamic = scene.createCube("Dynamic", glassMaterial);
        dynamic.setScale({1.0F, 2.0F, 0.01F});
        dynamic.addBoxCollider();
        dynamic.addRigidbody();
        const auto weak = scene.createCube("Weak", glassMaterial);
        weak.setScale({1.0F, 2.0F, 0.01F});
        weak.addBoxCollider();
        Engine::SceneEditor editor(scene);
        for (const auto &actor : {opaque, thick, dynamic}) {
            const auto entity = scene.findEntity(actor.id());
            EXPECT_FALSE(Engine::GlassDestructionSystem::onImpact(scene, entity, {}));
            EXPECT_FALSE(editor.has<Engine::GlassDestructionComponent>(entity));
        }
        const auto weakEntity = scene.findEntity(weak.id());
        EXPECT_FALSE(Engine::GlassDestructionSystem::onImpact(scene, weakEntity, {.energy = 1.0F}));
        EXPECT_FALSE(editor.has<Engine::GlassDestructionComponent>(weakEntity));
    }

    TEST(GlassDestruction, ThresholdAndInvalidHitsLeavePaneIntact) {
        Engine::Scene scene;
        const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Pane");
        const auto entity = scene.findEntity(pane.id());
        Engine::SceneEditor editor(scene);
        EXPECT_FALSE(Engine::GlassDestructionSystem::onImpact(scene, entity, {.energy = 1.0F}));
        EXPECT_FALSE(Engine::GlassDestructionSystem::onImpact(scene, entity, {.worldPosition = {3.0F, 0.0F, 0.0F}}));
        EXPECT_FALSE(Engine::GlassDestructionSystem::onImpact(scene, entity, {.radius = 0.0F}));
        EXPECT_FALSE(Engine::GlassDestructionSystem::onImpact(scene, entity, {.energy = std::numeric_limits<float>::quiet_NaN()}));
        EXPECT_FALSE(editor.read<Engine::GlassDestructionComponent>(entity).fractured);
        EXPECT_EQ(scene.objectCount(), 1);
    }

    TEST(GlassDestruction, HitscanCreatesActualHoleAndProgressiveDamage) {
        Engine::Scene scene;
        Engine::GlassDestructionComponent settings;
        settings.maxDynamicFragments = 0;
        const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Pane", settings);
        const auto entity = scene.findEntity(pane.id());
        Engine::SceneEditor editor(scene);
        ASSERT_TRUE(scene.physics().shootRay({0.0F, 0.0F, 2.0F}, {0.0F, 0.0F, -1.0F}));
        const auto before = editor.read<Engine::GlassDestructionComponent>(entity);
        ASSERT_TRUE(before.fractured);
        ASSERT_GT(before.fragments.size(), 4);
        float area = 0.0F;
        for (const auto &fragment : before.fragments) area += fragment.area;
        EXPECT_NEAR(area, 4.0F, 0.0001F);
        EXPECT_FALSE(before.bonds.empty());
        EXPECT_GT(attachedCount(before), 0);
        EXPECT_LT(attachedCount(before), before.fragments.size());
        EXPECT_FALSE(scene.physics().raycast({0.0F, 0.0F, 2.0F}, {0.0F, 0.0F, -1.0F}));
        const auto found = std::ranges::find_if(before.fragments, [](const auto &fragment) { return fragment.attached; });
        ASSERT_NE(found, before.fragments.end());
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, entity,
            {.worldPosition = {found->center.x, found->center.y, 0.0F}}));
        const auto &after = editor.read<Engine::GlassDestructionComponent>(entity);
        EXPECT_EQ(after.fragments.size(), before.fragments.size());
        EXPECT_LT(attachedCount(after), attachedCount(before));
        for (std::size_t i = 0; i < before.fragments.size(); ++i)
            EXPECT_EQ(after.fragments[i].polygon, before.fragments[i].polygon);
    }

    TEST(GlassDestruction, DebrisUsesPhysXAndExpiresWithinBudget) {
        Engine::Scene scene;
        Engine::GlassDestructionComponent settings;
        settings.maxDynamicFragments = 3;
        const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Pane", settings);
        pane.setPosition({0.0F, 3.0F, 0.0F});
        ASSERT_TRUE(scene.physics().shootRay({0.0F, 3.0F, 2.0F}, {0.0F, 0.0F, -1.0F}));
        Engine::SceneEditor editor(scene);
        std::vector<Engine::Entity> debris;
        editor.view<Engine::GlassDebrisComponent>([&](auto entity, const auto &) { debris.push_back(entity); });
        ASSERT_FALSE(debris.empty());
        EXPECT_LE(debris.size(), 3);
        const auto start = editor.read<Engine::Transform>(debris.front()).position;
        Engine::PhysicsSystem physics;
        for (int i = 0; i < 30; ++i) physics.update(scene, 1.0F / 60.0F);
        EXPECT_LT(editor.read<Engine::Transform>(debris.front()).position.y(), start.y());
        for (auto entity : debris) {
            EXPECT_GT(editor.read<Engine::RigidbodyComponent>(entity).mass, 0.0F);
            EXPECT_TRUE(editor.has<Engine::ColliderComponent>(entity));
            EXPECT_GT(editor.read<Engine::MeshRendererComponent>(entity).material.pbr.transmission, 0.9F);
        }
        Engine::GlassDestructionSystem::update(scene, 5.0F);
        for (auto entity : debris) EXPECT_FALSE(scene.valid(entity));
        physics.update(scene, 1.0F / 60.0F);
    }

    TEST(GlassDestruction, TransformedPaneAndSerializationPreserveTheHole) {
        Engine::Scene scene;
        Engine::GlassDestructionComponent settings;
        settings.maxDynamicFragments = 0;
        const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Pane", settings);
        pane.setPosition({3.0F, 2.0F, 0.0F});
        pane.setRotation({0.0F, 90.0F, 0.0F});
        pane.setScale({2.0F, 1.5F, 1.0F});
        ASSERT_TRUE(scene.physics().shootRay({5.0F, 2.0F, 0.0F}, {-1.0F, 0.0F, 0.0F}));
        EXPECT_FALSE(scene.physics().raycast({5.0F, 2.0F, 0.0F}, {-1.0F, 0.0F, 0.0F}));
        std::ostringstream output;
        Engine::SceneSerializer::save(scene, output);
        Engine::Scene restored;
        std::istringstream input(output.str());
        Engine::SceneSerializer::load(restored, input);
        EXPECT_FALSE(restored.physics().raycast({5.0F, 2.0F, 0.0F}, {-1.0F, 0.0F, 0.0F}));
        const auto restoredEntity = restored.findEntity(restored.findActor("Pane").id());
        Engine::SceneEditor editor(restored);
        EXPECT_TRUE(editor.read<Engine::GlassDestructionComponent>(restoredEntity).fractured);
    }


    TEST(GlassDestruction, DebrisCollidesWithFloorAndRepeatedReplacementKeepsHolesOpen) {
        Engine::Scene scene;
        const auto floor = scene.createActor("Floor");
        floor.setPosition({0.0F, -0.2F, 0.0F});
        floor.addBoxCollider({10.0F, 0.2F, 10.0F});
        Engine::GlassDestructionComponent settings;
        settings.fragmentLifetime = 10.0F;
        settings.maxDynamicFragments = 4;
        const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Pane", settings);
        pane.setPosition({0.0F, 2.0F, 0.0F});
        const auto entity = scene.findEntity(pane.id());
        Engine::SceneEditor editor(scene);
        Engine::PhysicsSystem physics;
        ASSERT_TRUE(scene.physics().shootRay({0.0F, 2.0F, 2.0F}, {0.0F, 0.0F, -1.0F}));
        for (int i = 0; i < 180; ++i) physics.update(scene, 1.0F / 60.0F);
        std::size_t count = 0;
        editor.view<Engine::GlassDebrisComponent, Engine::Transform>([&](auto, const auto &, const auto &transform) {
            ++count;
            EXPECT_GT(transform.position.y(), -0.05F);
            EXPECT_LT(transform.position.y(), 0.6F);
        });
        EXPECT_GT(count, 0);
        std::vector<glm::vec2> holes{{0.0F, 0.0F}};
        for (int iteration = 0; iteration < 8; ++iteration) {
            const auto glass = editor.read<Engine::GlassDestructionComponent>(entity);
            const auto fragment = std::ranges::find_if(glass.fragments, [](const auto &value) { return value.attached; });
            if (fragment == glass.fragments.end()) break;
            holes.push_back(fragment->center);
            ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, entity,
                {.worldPosition = {fragment->center.x, fragment->center.y + 2.0F, 0.0F}}));
            for (auto hole : holes) {
                const auto hit = scene.physics().raycast({hole.x, hole.y + 2.0F, 2.0F}, {0.0F, 0.0F, -1.0F});
                EXPECT_TRUE(!hit || hit->actor.id() != pane.id());
            }
        }
    }

    TEST(GlassDestruction, RemovingEveryBottomCellReleasesTopAndSides) {
        Engine::Scene scene;
        Engine::GlassDestructionComponent settings;
        settings.maxDynamicFragments = 0;
        const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Pane", settings);
        const auto entity = scene.findEntity(pane.id());
        Engine::SceneEditor editor(scene);
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, entity, {.radius = 0.03F}));
        EXPECT_GT(attachedCount(editor.read<Engine::GlassDestructionComponent>(entity)), 0);
        for (unsigned i = 0; i < settings.maxFragments; ++i) {
            const auto glass = editor.read<Engine::GlassDestructionComponent>(entity);
            const auto bottom = std::ranges::find_if(glass.fragments, [](const auto &fragment) {
                return fragment.attached && fragment.frameSupported;
            });
            if (bottom == glass.fragments.end()) break;
            ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, entity,
                {.worldPosition = {bottom->center.x, bottom->center.y, 0.0F}, .radius = 0.03F}));
        }
        EXPECT_TRUE(editor.read<Engine::GlassDestructionComponent>(entity).fullyDestroyed);
        EXPECT_EQ(attachedCount(editor.read<Engine::GlassDestructionComponent>(entity)), 0);
        EXPECT_FALSE(editor.has<Engine::ColliderComponent>(entity));
    }

    TEST(GlassDestruction, RemovingPhysicalBaseReleasesIntactPaneAndSurvivesSaveLoad) {
        Engine::Scene scene;
        const auto base = scene.createActor("Base");
        base.setPosition({0.0F, -0.1F, 0.0F});
        base.addBoxCollider({2.0F, 0.1F, 1.0F});
        Engine::GlassDestructionComponent settings;
        settings.maxDynamicFragments = 8;
        const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Pane", settings);
        pane.setPosition({0.0F, 1.0F, 0.0F});
        Engine::PhysicsSystem physics;
        physics.update(scene, 1.0F / 60.0F);
        Engine::SceneEditor editor(scene);
        EXPECT_TRUE(editor.read<Engine::GlassDestructionComponent>(scene.findEntity(pane.id())).hadPhysicalSupport);
        std::ostringstream saved;
        Engine::SceneSerializer::save(scene, saved);
        Engine::Scene restored;
        std::istringstream input(saved.str());
        Engine::SceneSerializer::load(restored, input);
        const auto entity = restored.findEntity(restored.findActor("Pane").id());
        restored.destroy(restored.findEntity(restored.findActor("Base").id()));
        Engine::PhysicsSystem restoredPhysics;
        restoredPhysics.update(restored, 1.0F / 60.0F);
        Engine::SceneEditor restoredEditor(restored);
        EXPECT_TRUE(restoredEditor.read<Engine::GlassDestructionComponent>(entity).fullyDestroyed);
        EXPECT_FALSE(restoredEditor.has<Engine::ColliderComponent>(entity));
        std::vector<Engine::Entity> debris;
        restoredEditor.view<Engine::GlassDebrisComponent>([&](auto shard, const auto &) { debris.push_back(shard); });
        ASSERT_FALSE(debris.empty());
        const auto start = restoredEditor.read<Engine::Transform>(debris.front()).position.y();
        for (int i = 0; i < 20; ++i) restoredPhysics.update(restored, 1.0F / 60.0F);
        EXPECT_LT(restoredEditor.read<Engine::Transform>(debris.front()).position.y(), start);
    }

    TEST(GlassDestruction, RepeatedDamageKeepsPackedRenderGeometryAndSeparateHoleCollision) {
        Engine::Scene scene;
        Engine::GlassDestructionComponent settings;
        settings.maxDynamicFragments = 0;
        const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Pane", settings);
        const auto entity = scene.findEntity(pane.id());
        Engine::SceneEditor editor(scene);
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, entity, {}));
        const auto handle = editor.read<Engine::MeshRendererComponent>(entity).mesh;
        const auto count = handle.source()->indexCount();
        EXPECT_EQ(handle.resource()->metadata.sections.size(),
            editor.read<Engine::GlassDestructionComponent>(entity).fragments.size());
        const auto glass = editor.read<Engine::GlassDestructionComponent>(entity);
        const auto next = std::ranges::find_if(glass.fragments, [](const auto &fragment) { return fragment.attached; });
        ASSERT_NE(next, glass.fragments.end());
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, entity,
            {.worldPosition = {next->center.x, next->center.y, 0.0F}}));
        EXPECT_EQ(editor.read<Engine::MeshRendererComponent>(entity).mesh.resource(), handle.resource());
        EXPECT_EQ(handle.source()->indexCount(), count);
        EXPECT_FALSE(scene.physics().raycast({0.0F, 0.0F, 2.0F}, {0.0F, 0.0F, -1.0F}));
        std::ostringstream saved;
        Engine::SceneSerializer::save(scene, saved);
        Engine::Scene restored;
        std::istringstream input(saved.str());
        Engine::SceneSerializer::load(restored, input);
        EXPECT_FALSE(restored.physics().raycast({0.0F, 0.0F, 2.0F}, {0.0F, 0.0F, -1.0F}));
        Engine::SceneEditor restoredEditor(restored);
        const auto restoredEntity = restored.findEntity(restored.findActor("Pane").id());
        EXPECT_EQ(restoredEditor.read<Engine::MeshRendererComponent>(restoredEntity).mesh.resource()->metadata.sections.size(),
            glass.fragments.size());
    }

    TEST(GlassDestruction, DebrisExpiresDuringPhysicsStepWithoutStaleActors) {
        Engine::Scene scene;
        Engine::GlassDestructionComponent settings;
        settings.fragmentLifetime = 0.05F;
        settings.maxDynamicFragments = 2;
        const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Pane", settings);
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, scene.findEntity(pane.id()), {}));
        Engine::PhysicsSystem physics;
        for (int i = 0; i < 12; ++i) ASSERT_NO_THROW(physics.update(scene, 1.0F / 60.0F));
        std::size_t count = 0;
        scene.editor().view<Engine::GlassDebrisComponent>([&](auto, const auto &) { ++count; });
        EXPECT_EQ(count, 0);
    }

    TEST(GlassDestruction, HugeImpactRemovesAllCollisionAndInvalidSettingsAreRejected) {
        Engine::Scene scene;
        Engine::GlassDestructionComponent settings;
        settings.maxDynamicFragments = 0;
        const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Pane", settings);
        const auto entity = scene.findEntity(pane.id());
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, entity, {.energy = 1000.0F, .radius = 10.0F}));
        Engine::SceneEditor editor(scene);
        EXPECT_TRUE(editor.read<Engine::GlassDestructionComponent>(entity).fullyDestroyed);
        EXPECT_FALSE(editor.has<Engine::ColliderComponent>(entity));
        EXPECT_FALSE(scene.physics().raycast({0.0F, 0.0F, 2.0F}, {0.0F, 0.0F, -1.0F}));
        settings.maxFragments = 1000000;
        EXPECT_THROW((void)Engine::GlassDestructionSystem::createPane(scene, "Invalid", settings), std::invalid_argument);
    }
}
