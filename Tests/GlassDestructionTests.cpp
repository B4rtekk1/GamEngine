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

    TEST(GlassDestruction, LegacyGlassCubeIsPreparedDuringLoadBeforeFirstShot) {
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
        EXPECT_TRUE(hadDestruction);
        const auto loadedMesh = editor.read<Engine::MeshRendererComponent>(entity).mesh;
        EXPECT_FALSE(editor.read<Engine::GlassDestructionComponent>(entity).fractured);
        ASSERT_TRUE(loaded.physics().shootRay({0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, -1.0F}));
        ASSERT_TRUE(editor.has<Engine::GlassDestructionComponent>(entity));
        EXPECT_GT(loadedMesh.resource()->metadata.sections.size(), 1U);
        EXPECT_EQ(editor.read<Engine::MeshRendererComponent>(entity).mesh.resource()->metadata.sections.size(),
            editor.read<Engine::GlassDestructionComponent>(entity).fragments.size()+1);
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
        EXPECT_FALSE(Engine::GlassDestructionSystem::onImpact(scene, entity, {.energy = 0.5F}));
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
            EXPECT_FALSE(editor.has<Engine::MeshRendererComponent>(entity));
            EXPECT_TRUE(editor.has<Engine::GlassFragmentBindingComponent>(entity));
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
        settings.supportMode = Engine::GlassSupportMode::Bottom;
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
        settings.supportMode = Engine::GlassSupportMode::Bottom;
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
            editor.read<Engine::GlassDestructionComponent>(entity).fragments.size() + 1);
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
            glass.fragments.size() + 1);
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

    TEST(GlassDestruction, BoundDebrisSurvivesSaveLoadAndPaneRemoval) {
        Engine::Scene scene;
        const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Pane");
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, scene.findEntity(pane.id()), {}));
        std::ostringstream saved;
        Engine::SceneSerializer::save(scene, saved);
        Engine::Scene loaded;
        std::istringstream input(saved.str());
        Engine::SceneSerializer::load(loaded, input);
        const auto paneEntity = loaded.findEntity(loaded.findActor("Pane").id());
        Engine::SceneEditor editor(loaded);
        std::vector<Engine::Entity> shards;
        editor.view<Engine::GlassFragmentBindingComponent>([&](auto entity, const auto &binding) {
            shards.push_back(entity);
            EXPECT_EQ(binding.paneEntity, paneEntity);
            EXPECT_EQ(editor.read<Engine::GlassDestructionComponent>(paneEntity).fragments[binding.fragmentIndex].debrisEntity, entity);
            EXPECT_TRUE(std::get<Engine::MeshCollider>(editor.read<Engine::ColliderComponent>(entity).shape).mesh);
        });
        ASSERT_FALSE(shards.empty());
        loaded.destroy(paneEntity);
        for (auto entity : shards) {
            EXPECT_TRUE(loaded.valid(entity));
            EXPECT_TRUE(editor.has<Engine::MeshRendererComponent>(entity));
            EXPECT_FALSE(editor.has<Engine::GlassFragmentBindingComponent>(entity));
        }
        Engine::PhysicsSystem physics;
        EXPECT_NO_THROW(physics.update(loaded, 1.0F / 60.0F));
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
    TEST(GlassDestruction, FirstWeakImpactFixesGraphAndLaterImpactsKeepGeometry) {
        Engine::Scene scene;
        const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Pane");
        const auto entity = scene.findEntity(pane.id());
        Engine::SceneEditor editor(scene);
        const auto mesh = editor.read<Engine::MeshRendererComponent>(entity).mesh;
        const auto topology = editor.renderTopologyRevision();
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, entity,
            {.worldPosition = {-0.5F, 0, 0}, .energy = 2}));
        const auto hitMesh = editor.read<Engine::MeshRendererComponent>(entity).mesh;
        const auto hitTopology = editor.renderTopologyRevision();
        EXPECT_NE(hitMesh.resource(), mesh.resource());
        EXPECT_GT(hitTopology, topology);
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, entity,
            {.worldPosition = {0.5F, 0, 0}, .energy = 2}));
        const auto &glass = editor.read<Engine::GlassDestructionComponent>(entity);
        EXPECT_FALSE(glass.fractured);
        EXPECT_EQ(glass.renderState, Engine::GlassRenderState::Cracked);
        ASSERT_EQ(glass.cracks.size(), 2);
        EXPECT_NE(glass.cracks[0].position, glass.cracks[1].position);
        EXPECT_EQ(editor.read<Engine::MeshRendererComponent>(entity).mesh.resource(), hitMesh.resource());
        EXPECT_EQ(editor.renderTopologyRevision(), hitTopology);
        EXPECT_TRUE(std::holds_alternative<Engine::BoxCollider>(editor.read<Engine::ColliderComponent>(entity).shape));
        EXPECT_TRUE(scene.physics().raycast({-0.5F, 0, 2}, {0, 0, -1}));
        std::ostringstream output;
        Engine::SceneSerializer::save(scene, output);
        Engine::Scene restored;
        std::istringstream input(output.str());
        Engine::SceneSerializer::load(restored, input);
        Engine::SceneEditor loaded(restored);
        const auto restoredEntity = restored.findEntity(restored.findActor("Pane").id());
        const auto &saved = loaded.read<Engine::GlassDestructionComponent>(restoredEntity);
        ASSERT_EQ(saved.cracks.size(), 2);
        EXPECT_EQ(saved.cracks[0].position, glass.cracks[0].position);
        EXPECT_EQ(saved.supportMode, Engine::GlassSupportMode::Frame);
        EXPECT_FALSE(saved.fractured);
        EXPECT_EQ(loaded.read<Engine::MeshRendererComponent>(restoredEntity).mesh.resource()->metadata.sections.size(),
            loaded.read<Engine::GlassDestructionComponent>(restoredEntity).fragments.size() + 1);
    }

    TEST(GlassDestruction, BranchingTemplateConservesAreaAndHasGeometricBondStrength) {
        Engine::Scene scene;
        Engine::GlassDestructionComponent settings;
        settings.size = {3.0F, 1.5F}; settings.maxFragments = 128;
        const auto first = Engine::GlassDestructionSystem::createPane(scene, "First", settings);
        settings.thickness *= 2;
        const auto second = Engine::GlassDestructionSystem::createPane(scene, "Second", settings);
        Engine::SceneEditor editor(scene);
        const auto &a = editor.read<Engine::GlassDestructionComponent>(scene.findEntity(first.id()));
        const auto &b = editor.read<Engine::GlassDestructionComponent>(scene.findEntity(second.id()));
        float area = 0;
        for (const auto &fragment : a.fragments) {
            area += fragment.area;
            EXPECT_GT(fragment.area, 0);
            for (std::size_t i = 0; i < fragment.polygon.size(); ++i) {
                const auto p = fragment.polygon[i], q = fragment.polygon[(i+1)%fragment.polygon.size()];
                const auto r = fragment.polygon[(i+2)%fragment.polygon.size()];
                EXPECT_GE((q.x-p.x)*(r.y-q.y)-(q.y-p.y)*(r.x-q.x), -0.00001F);
            }
        }
        EXPECT_NEAR(area, 4.5F, 0.0001F);
        EXPECT_LE(a.fragments.size(), settings.maxFragments);
        ASSERT_FALSE(a.bonds.empty()); ASSERT_EQ(a.bonds.size(), b.bonds.size());
        for (std::size_t i = 0; i < a.bonds.size(); ++i) {
            const auto &bond = a.bonds[i];
            EXPECT_NEAR(bond.length, glm::distance(bond.edgeStart, bond.edgeEnd), 0.00001F);
            EXPECT_NEAR(b.bonds[i].strength, bond.strength*2, 0.0001F);
        }
        const auto mesh = Engine::GlassDestructionSystem::renderMesh(a);
        std::size_t expectedVertices = 0;
        for (const auto &fragment : a.fragments) expectedVertices += fragment.polygon.size()*6;
        EXPECT_EQ(mesh->vertices.size(), expectedVertices + 24);
    }

    TEST(GlassDestruction, PreparedPhysicsShapesToggleWithoutCookingOrReplacingPaneActor) {
        Engine::Scene scene;
        Engine::GlassDestructionComponent settings; settings.maxDynamicFragments = 0;
        const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Pane", settings);
        Engine::PhysicsSystem physics;
        physics.update(scene, 1.0F/60);
        const auto before = physics.diagnostics();
        ASSERT_GT(before.convexCookCount, 0);
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, scene.findEntity(pane.id()), {}));
        physics.update(scene, 1.0F/60);
        const auto after = physics.diagnostics();
        EXPECT_EQ(before.convexCookCount, after.convexCookCount);
        EXPECT_EQ(before.triangleCookCount, after.triangleCookCount);
        EXPECT_EQ(before.actorCreations, after.actorCreations);
        EXPECT_GT(after.glassShapeUpdates, before.glassShapeUpdates);
        EXPECT_FALSE(physics.raycast(scene, {0,0,2}, {0,0,-1}));
    }

    TEST(GlassDestruction, PaneAndGlobalDebrisBudgetsAreIndependentAndEnergyIsBounded) {
        Engine::Scene scene;
        scene.glassSimulationSettings().maxDynamicFragmentsGlobal = 3;
        Engine::GlassDestructionComponent settings; settings.maxDynamicFragments = 2;
        const auto first = Engine::GlassDestructionSystem::createPane(scene, "First", settings);
        const auto second = Engine::GlassDestructionSystem::createPane(scene, "Second", settings);
        second.setPosition({4,0,0});
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, scene.findEntity(first.id()), {}));
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, scene.findEntity(second.id()), {.worldPosition={4,0,0}}));
        Engine::SceneEditor editor(scene);
        std::size_t count = 0, firstCount = 0, secondCount = 0;
        float kinetic = 0;
        editor.view<Engine::GlassDebrisComponent, Engine::GlassFragmentBindingComponent,
            Engine::RigidbodyState, Engine::RigidbodyComponent>([&](auto, const auto &, const auto &binding, const auto &state, const auto &body) {
                ++count; firstCount += binding.paneEntity == scene.findEntity(first.id());
                secondCount += binding.paneEntity == scene.findEntity(second.id());
                const auto v = state.linearVelocity.native(), w = glm::radians(state.angularVelocity.native());
                EXPECT_TRUE(std::isfinite(glm::length(v))); EXPECT_TRUE(std::isfinite(glm::length(w)));
                EXPECT_LE(glm::length(v), scene.glassSimulationSettings().maxLinearSpeed+0.001F);
                EXPECT_LE(glm::length(w), scene.glassSimulationSettings().maxAngularSpeed+0.001F);
                kinetic += 0.5F*body.mass*glm::dot(v,v);
            });
        EXPECT_EQ(count, 3); EXPECT_EQ(firstCount, 2); EXPECT_EQ(secondCount, 1);
        EXPECT_LE(kinetic, 80.0F*scene.glassSimulationSettings().kineticEnergyFraction+0.001F);
        EXPECT_FALSE(scene.glassParticleBursts().empty());
    }

    TEST(GlassDestruction, LaminatedAndArmoredGlassRetainWeakDamageWhileTemperedCrumbles) {
        Engine::Scene scene;
        Engine::SceneEditor editor(scene);
        Engine::GlassDestructionComponent settings; settings.maxDynamicFragments = 0;
        settings.type = Engine::GlassType::Laminated;
        const auto laminated = Engine::GlassDestructionSystem::createPane(scene, "Laminated", settings);
        const auto l = scene.findEntity(laminated.id());
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, l, {}));
        EXPECT_FALSE(editor.read<Engine::GlassDestructionComponent>(l).fractured);
        EXPECT_EQ(attachedCount(editor.read<Engine::GlassDestructionComponent>(l)),
            editor.read<Engine::GlassDestructionComponent>(l).fragments.size());
        settings.type = Engine::GlassType::Armored; settings.layers = 3;
        const auto armored = Engine::GlassDestructionSystem::createPane(scene, "Armored", settings);
        const auto a = scene.findEntity(armored.id());
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, a, {}));
        EXPECT_FALSE(editor.read<Engine::GlassDestructionComponent>(a).fractured);
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, a, {}));
        EXPECT_TRUE(editor.read<Engine::GlassDestructionComponent>(a).fractured);
        settings.type = Engine::GlassType::Tempered;
        const auto tempered = Engine::GlassDestructionSystem::createPane(scene, "Tempered", settings);
        const auto t = scene.findEntity(tempered.id());
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, t, {}));
        EXPECT_TRUE(editor.read<Engine::GlassDestructionComponent>(t).fullyDestroyed);
        EXPECT_FALSE(editor.has<Engine::ColliderComponent>(t));
        EXPECT_FALSE(scene.glassParticleBursts().empty());
    }

    TEST(GlassDestruction, StationaryBottomSupportDoesNotRepeatRaycastsForMovingDebris) {
        Engine::Scene scene;
        const auto base = scene.createActor("Base"); base.setPosition({0,-0.1F,0}); base.addBoxCollider({2,0.1F,1});
        Engine::GlassDestructionComponent settings; settings.supportMode = Engine::GlassSupportMode::Bottom;
        const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Pane", settings); pane.setPosition({0,1,0});
        const auto entity = scene.findEntity(pane.id());
        const auto ball = scene.createActor("Ball"); ball.setPosition({4,3,0}); ball.addSphereCollider(); ball.addRigidbody();
        Engine::PhysicsSystem physics; physics.update(scene, 1.0F/60);
        Engine::SceneEditor editor(scene);
        const auto rays = editor.read<Engine::GlassDestructionComponent>(entity).supportRaycasts;
        EXPECT_EQ(rays, 3);
        for (unsigned i=0; i<10; ++i) physics.update(scene, 1.0F/60);
        EXPECT_EQ(editor.read<Engine::GlassDestructionComponent>(entity).supportRaycasts, rays);
        scene.destroy(scene.findEntity(base.id())); physics.update(scene, 1.0F/60);
        EXPECT_TRUE(editor.read<Engine::GlassDestructionComponent>(entity).fullyDestroyed);
    }

    TEST(GlassDestruction, BurstBudgetLimitsNewBodiesAndResetsAtTheNextSimulationStep) {
        Engine::Scene scene;
        scene.glassSimulationSettings().maxNewDynamicFragmentsPerStep = 2;
        scene.glassSimulationSettings().adaptivePhysicsBudget = false;
        Engine::GlassDestructionComponent settings; settings.maxDynamicFragments = 8;
        const auto first = Engine::GlassDestructionSystem::createPane(scene, "First", settings);
        const auto second = Engine::GlassDestructionSystem::createPane(scene, "Second", settings);
        const auto third = Engine::GlassDestructionSystem::createPane(scene, "Third", settings);
        second.setPosition({4,0,0}); third.setPosition({8,0,0});
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, scene.findEntity(first.id()), {}));
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, scene.findEntity(second.id()), {.worldPosition={4,0,0}}));
        Engine::SceneEditor editor(scene);
        const auto countBodies = [&] {
            unsigned count = 0;
            editor.view<Engine::GlassDebrisComponent>([&](auto, const auto &) { ++count; });
            return count;
        };
        EXPECT_EQ(countBodies(), 2);
        EXPECT_FALSE(scene.glassParticleBursts().empty());
        EXPECT_FALSE(scene.physics().raycast({4,0,2}, {0,0,-1}));
        Engine::GlassDestructionSystem::update(scene, 1.0F/60);
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, scene.findEntity(third.id()), {.worldPosition={8,0,0}}));
        EXPECT_EQ(countBodies(), 4);
        EXPECT_FALSE(scene.physics().raycast({8,0,2}, {0,0,-1}));
    }

    TEST(GlassDestruction, CrackHistoryStaysBoundedAndRetainsNewImpacts) {
        Engine::Scene scene;
        Engine::GlassDestructionComponent settings; settings.type = Engine::GlassType::Armored; settings.layers = 16;
        const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Pane", settings);
        const auto entity = scene.findEntity(pane.id());
        for (unsigned i=0; i<140; ++i)
            ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, entity, {.energy=1}));
        Engine::SceneEditor editor(scene);
        const auto &glass = editor.read<Engine::GlassDestructionComponent>(entity);
        EXPECT_FALSE(glass.fractured); EXPECT_EQ(glass.impactCount, 140); EXPECT_EQ(glass.cracks.size(), 128);
        EXPECT_EQ(glass.cracks.back().seed, settings.seed+140U*1664525U);
    }

    TEST(GlassDestruction, TransformedPreparedShapesShareCookingWithDynamicShards) {
        Engine::Scene scene;
        Engine::GlassDestructionComponent settings; settings.maxDynamicFragments=3;
        const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Pane", settings);
        pane.setPosition({3,4,5}); pane.setRotation({31,27,13}); pane.setScale({1.3F,1.7F,0.9F});
        Engine::PhysicsSystem physics; physics.update(scene, 1.0F/60);
        const auto before = physics.diagnostics();
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, scene.findEntity(pane.id()), {.worldPosition={3,4,5}}));
        physics.update(scene, 1.0F/60);
        EXPECT_EQ(physics.diagnostics().convexCookCount, before.convexCookCount);
    }

    TEST(GlassDestruction, IdenticalPanesShareGeometryAndCookingButKeepIndependentDamage) {
        Engine::Scene scene; Engine::SceneEditor editor(scene);
        Engine::GlassDestructionComponent settings; settings.maxDynamicFragments=0;
        const auto first = Engine::GlassDestructionSystem::createPane(scene, "First", settings);
        const auto second = Engine::GlassDestructionSystem::createPane(scene, "Second", settings);
        second.setPosition({4,0,0});
        const auto a=scene.findEntity(first.id()), b=scene.findEntity(second.id());
        EXPECT_EQ(editor.read<Engine::MeshRendererComponent>(a).mesh.resource(), editor.read<Engine::MeshRendererComponent>(b).mesh.resource());
        EXPECT_EQ(editor.read<Engine::GlassDestructionComponent>(a).fragments.front().collisionGeometry,
            editor.read<Engine::GlassDestructionComponent>(b).fragments.front().collisionGeometry);
        Engine::PhysicsSystem physics; physics.update(scene,1.0F/60);
        EXPECT_EQ(physics.diagnostics().convexCookCount, editor.read<Engine::GlassDestructionComponent>(a).fragments.size());
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene,a,{}));
        EXPECT_FALSE(editor.read<Engine::GlassDestructionComponent>(b).fractured);
        EXPECT_TRUE(editor.read<Engine::GlassDestructionComponent>(b).cracks.empty());
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene,b,{.worldPosition={4,0,0}}));
        EXPECT_EQ(editor.read<Engine::MeshRendererComponent>(a).mesh.resource(),editor.read<Engine::MeshRendererComponent>(b).mesh.resource());
    }

    TEST(GlassDestruction, SharedGraphEdgesControlCrackMaskAndSideExposure) {
        Engine::Scene scene;
        const auto pane=Engine::GlassDestructionSystem::createPane(scene,"Graph");
        Engine::SceneEditor editor(scene);
        auto glass=editor.read<Engine::GlassDestructionComponent>(scene.findEntity(pane.id()));
        ASSERT_FALSE(glass.bonds.empty());
        const auto bond=glass.bonds.front();
        auto edgeIndex=[&](unsigned index) {
            const auto &polygon=glass.fragments[index].polygon;
            for (unsigned edge=0; edge<polygon.size(); ++edge) {
                const auto a=polygon[edge], b=polygon[(edge+1)%polygon.size()];
                if ((glm::distance(a,bond.edgeStart)<0.0001F && glm::distance(b,bond.edgeEnd)<0.0001F) ||
                    (glm::distance(b,bond.edgeStart)<0.0001F && glm::distance(a,bond.edgeEnd)<0.0001F)) return edge;
            }
            return ~0U;
        };
        const auto first=edgeIndex(bond.first), second=edgeIndex(bond.second);
        ASSERT_NE(first,~0U); ASSERT_NE(second,~0U);
        EXPECT_FALSE(Engine::GlassDestructionSystem::edgeExposed(glass,bond.first,first));
        EXPECT_FALSE(Engine::GlassDestructionSystem::edgeExposed(glass,bond.second,second));
        glass.bonds.front().broken=true;
        EXPECT_TRUE(Engine::GlassDestructionSystem::edgeExposed(glass,bond.first,first));
        EXPECT_TRUE(Engine::GlassDestructionSystem::edgeExposed(glass,bond.second,second));
        glass.bonds.front().broken=false;
        glass.fragments[bond.second].attached=false;
        EXPECT_TRUE(Engine::GlassDestructionSystem::edgeExposed(glass,bond.first,first));
        glass.bonds.front().broken=true;
        // Broken edges must appear even outside the impact footprint.
        const auto segments=Engine::GlassDestructionSystem::crackSegments(glass,{{0.8F,0.8F},0.1F,0.4F,7});
        EXPECT_TRUE(std::ranges::any_of(segments,[&](const auto &segment) {
            return segment.start==bond.edgeStart && segment.end==bond.edgeEnd && segment.intensity==1.0F;
        }));
    }

    TEST(GlassDestruction, OffCenterGraphAndSurfaceTipsSurviveSerialization) {
        Engine::Scene scene;
        const auto pane=Engine::GlassDestructionSystem::createPane(scene,"Offset");
        const auto entity=scene.findEntity(pane.id());
        Engine::PhysicsSystem physics; physics.update(scene,1.0F/60);
        const auto initialCookCount=physics.diagnostics().convexCookCount;
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene,entity,
            {.worldPosition={0.65F,-0.45F,0},.energy=2}));
        physics.update(scene,1.0F/60);
        EXPECT_GT(physics.diagnostics().convexCookCount,initialCookCount);
        const auto hitCookCount=physics.diagnostics().convexCookCount;
        physics.update(scene,1.0F/60);
        EXPECT_EQ(physics.diagnostics().convexCookCount,hitCookCount);
        Engine::SceneEditor editor(scene);
        const auto glass=editor.read<Engine::GlassDestructionComponent>(entity);
        const auto segments=Engine::GlassDestructionSystem::crackSegments(glass,glass.cracks.front());
        ASSERT_FALSE(segments.empty());
        bool curved=false;
        for (const auto &segment:segments) {
            EXPECT_LE(glm::abs(segment.start).x,glass.size.x*0.5F+0.0001F);
            EXPECT_LE(glm::abs(segment.start).y,glass.size.y*0.5F+0.0001F);
            EXPECT_LE(glm::abs(segment.end).x,glass.size.x*0.5F+0.0001F);
            EXPECT_LE(glm::abs(segment.end).y,glass.size.y*0.5F+0.0001F);
            for (const auto &next:segments) if (segment.end==next.start) {
                const auto a=segment.end-segment.start, b=next.end-next.start;
                curved |= std::abs(a.x*b.y-a.y*b.x)>1.0e-8F;
            }
        }
        EXPECT_TRUE(curved);
        std::ostringstream output; Engine::SceneSerializer::save(scene,output);
        Engine::Scene restored; std::istringstream input(output.str());
        Engine::SceneSerializer::load(restored,input);
        Engine::SceneEditor loaded(restored);
        const auto &saved=loaded.read<Engine::GlassDestructionComponent>(restored.findEntity(restored.findActor("Offset").id()));
        ASSERT_EQ(saved.fragments.size(),glass.fragments.size());
        for (unsigned i=0; i<glass.fragments.size(); ++i) EXPECT_EQ(saved.fragments[i].polygon,glass.fragments[i].polygon);
        const auto replay=Engine::GlassDestructionSystem::crackSegments(saved,saved.cracks.front());
        ASSERT_EQ(replay.size(),segments.size());
        for (unsigned i=0; i<segments.size(); ++i) {
            EXPECT_EQ(replay[i].start,segments[i].start); EXPECT_EQ(replay[i].end,segments[i].end);
            EXPECT_EQ(replay[i].intensity,segments[i].intensity);
        }
    }

}
