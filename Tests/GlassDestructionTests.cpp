#include "Engine/ECS/Components/RigidbodyRuntime.h"
#include "Engine/Physics/GlassDestructionSystem.h"
#include "Engine/Physics/PhysicsSystem.h"
#include "Engine/Scene/SceneEditor.h"
#include "Engine/Scene/SceneSerializer.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <glm/gtc/constants.hpp>
#include <gtest/gtest.h>
#include <limits>
#include <sstream>

namespace {
std::size_t attachedCount(const Engine::GlassDestructionComponent& glass) {
    return std::ranges::count_if(glass.fragments, [](const auto& fragment) { return fragment.attached; });
}

float frontFaceArea(const Engine::Mesh& mesh) {
    float area = 0;
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
        const auto &a = mesh.vertices[mesh.indices[i]], &b = mesh.vertices[mesh.indices[i + 1]],
                   &c = mesh.vertices[mesh.indices[i + 2]];
        if (a.normal.z() < 0.9F || b.normal.z() < 0.9F || c.normal.z() < 0.9F)
            continue;
        const auto ab = b.position.native() - a.position.native(), ac = c.position.native() - a.position.native();
        const float triangle = glm::cross(ab, ac).z * 0.5F;
        EXPECT_GT(triangle, 0);
        area += triangle;
    }
    return area;
}

TEST(GlassDestruction, ConcaveCrackBoundariesHaveExactRenderAndCollisionArea) {
    Engine::Scene scene;
    for (unsigned seed : {1U, 7U, 42U, 1337U}) {
        Engine::GlassDestructionComponent settings;
        settings.seed = seed;
        settings.maxFragments = 128;
        settings.size = {3, 1.5F};
        const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Curved", settings);
        Engine::SceneEditor editor(scene);
        const auto& glass = editor.read<Engine::GlassDestructionComponent>(scene.findEntity(pane.id()));
        float area = 0;
        unsigned concave = 0;
        for (const auto& fragment : glass.fragments) {
            area += fragment.area;
            ASSERT_TRUE(fragment.collisionGeometry);
            EXPECT_NEAR(frontFaceArea(*fragment.collisionGeometry), fragment.area, 0.00001F);
            const auto& parts = fragment.collisionGeometry->convexCollisionParts;
            concave += !parts.empty();
            if (parts.empty())
                continue;
            float collisionArea = 0;
            for (const auto& part : parts) {
                collisionArea += frontFaceArea(*part);
                const auto count = part->vertices.size() / 6;
                ASSERT_GE(count, 3U);
                for (std::size_t i = 0; i < count; ++i) {
                    const auto a = part->vertices[i].position.native(),
                               b = part->vertices[(i + 1) % count].position.native(),
                               c = part->vertices[(i + 2) % count].position.native();
                    EXPECT_GE(glm::cross(b - a, c - b).z, -1.0e-9F);
                }
            }
            EXPECT_NEAR(collisionArea, fragment.area, 0.00001F);
            EXPECT_LT(fragment.polygon.size(), 128U); // Renderer side-exposure bit capacity.
        }
        EXPECT_GT(concave, glass.fragments.size() / 2);
        EXPECT_NEAR(area, 4.5F, 0.0001F);
    }
}

TEST(GlassDestruction, ConcaveRigidShardRaycastsDoNotFillItsNotches) {
    Engine::Scene source;
    const auto pane = Engine::GlassDestructionSystem::createPane(source, "Source");
    Engine::SceneEditor sourceEditor(source);
    const auto& glass = sourceEditor.read<Engine::GlassDestructionComponent>(source.findEntity(pane.id()));
    const auto selected = std::ranges::max_element(glass.fragments, {}, [](const auto& fragment) {
        return fragment.collisionGeometry->convexCollisionParts.empty() ? 0.0F : fragment.area;
    });
    ASSERT_FALSE(selected->collisionGeometry->convexCollisionParts.empty());
    Engine::Scene scene;
    const auto shard = scene.createMesh("Shard", selected->collisionGeometry);
    Engine::SceneEditor editor(scene);
    const auto entity = scene.findEntity(shard.id());
    editor.add<Engine::ColliderComponent>(
        entity, Engine::ColliderComponent{.shape = Engine::MeshCollider{selected->collisionGeometry}, .offset = {}});
    editor.add<Engine::RigidbodyComponent>(
        entity, Engine::RigidbodyComponent{.type = Engine::RigidbodyType::Kinematic, .useGravity = false});
    editor.add<Engine::GlassDebrisComponent>(entity);
    std::ostringstream saved;
    Engine::SceneSerializer::save(scene, saved);
    Engine::Scene loaded;
    std::istringstream input(saved.str());
    Engine::SceneSerializer::load(loaded, input);
    Engine::SceneEditor loadedEditor(loaded);
    const auto loadedEntity = loaded.findEntity(loaded.findActor("Shard").id());
    const auto& loadedCollider = loadedEditor.read<Engine::ColliderComponent>(loadedEntity);
    ASSERT_FALSE(std::get<Engine::MeshCollider>(loadedCollider.shape).mesh->convexCollisionParts.empty());
    Engine::PhysicsSystem physics;
    auto inside = [&](glm::vec2 point) {
        bool result = false;
        const auto& polygon = selected->polygon;
        for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
            const auto a = polygon[j], b = polygon[i];
            if ((a.y > point.y) != (b.y > point.y) && point.x < (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x)
                result = !result;
        }
        return result;
    };
    // Probe both sides of every curved boundary, including concave notches.
    unsigned hits = 0, misses = 0;
    for (std::size_t i = 0; i < selected->polygon.size(); ++i) {
        const auto a = selected->polygon[i], b = selected->polygon[(i + 1) % selected->polygon.size()], d = b - a;
        if (glm::length(d) < 0.002F)
            continue;
        const auto normal = glm::vec2{-d.y, d.x} / glm::length(d);
        for (float sign : {-1.0F, 1.0F}) {
            const auto point = (a + b) * 0.5F + normal * 0.001F * sign;
            const auto local = point - selected->center;
            const auto hit = physics.raycast(scene, {local.x, local.y, 1}, {0, 0, -1}, 2);
            const auto restoredHit = loaded.physics().raycast({local.x, local.y, 1}, {0, 0, -1}, 2);
            EXPECT_EQ(bool(restoredHit), inside(point));
            EXPECT_EQ(bool(hit), inside(point));
            if (hit)
                ++hits;
            else
                ++misses;
        }
    }
    EXPECT_GT(hits, 3U);
    EXPECT_GT(misses, 3U);
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
              editor.read<Engine::GlassDestructionComponent>(entity).fragments.size() + 1);
    const auto& glass = editor.read<Engine::GlassDestructionComponent>(entity);
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
    for (const auto& actor : {opaque, thick, dynamic}) {
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
    EXPECT_FALSE(
        Engine::GlassDestructionSystem::onImpact(scene, entity, {.energy = std::numeric_limits<float>::quiet_NaN()}));
    EXPECT_FALSE(editor.read<Engine::GlassDestructionComponent>(entity).fractured);
    EXPECT_EQ(scene.objectCount(), 1);
}

TEST(GlassDestruction, GlancingBulletDepositsLessFractureEnergy) {
    Engine::Scene normalScene;
    Engine::Scene glancingScene;
    const auto normalPane = Engine::GlassDestructionSystem::createPane(normalScene, "Normal");
    const auto glancingPane = Engine::GlassDestructionSystem::createPane(glancingScene, "Glancing");
    const Engine::GlassImpact normal{.worldPosition = {0.0F, 0.0F, 0.0F},
                                     .worldDirection = {0.0F, 0.0F, -1.0F},
                                     .worldNormal = {0.0F, 0.0F, 1.0F},
                                     .energy = 10.0F,
                                     .radius = 0.1F};
    const Engine::GlassImpact glancing{.worldPosition = {0.0F, 0.0F, 0.0F},
                                       .worldDirection = {0.995F, 0.0F, -0.1F},
                                       .worldNormal = {0.0F, 0.0F, 1.0F},
                                       .energy = 10.0F,
                                       .radius = 0.1F};
    ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(normalScene, normalScene.findEntity(normalPane.id()), normal));
    ASSERT_TRUE(
        Engine::GlassDestructionSystem::onImpact(glancingScene, glancingScene.findEntity(glancingPane.id()), glancing));
    Engine::SceneEditor normalEditor(normalScene);
    Engine::SceneEditor glancingEditor(glancingScene);
    EXPECT_GT(
        normalEditor.read<Engine::GlassDestructionComponent>(normalScene.findEntity(normalPane.id())).accumulatedEnergy,
        glancingEditor.read<Engine::GlassDestructionComponent>(glancingScene.findEntity(glancingPane.id()))
            .accumulatedEnergy);
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
    for (const auto& fragment : before.fragments)
        area += fragment.area;
    EXPECT_NEAR(area, 4.0F, 0.0001F);
    EXPECT_FALSE(before.bonds.empty());
    EXPECT_GT(attachedCount(before), 0);
    EXPECT_LT(attachedCount(before), before.fragments.size());
    EXPECT_FALSE(scene.physics().raycast({0.0F, 0.0F, 2.0F}, {0.0F, 0.0F, -1.0F}));
    const auto found = std::ranges::find_if(before.fragments, [](const auto& fragment) { return fragment.attached; });
    ASSERT_NE(found, before.fragments.end());
    ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, entity,
                                                         {.worldPosition = {found->center.x, found->center.y, 0.0F}}));
    const auto& after = editor.read<Engine::GlassDestructionComponent>(entity);
    EXPECT_GT(after.fragments.size(), before.fragments.size());
    EXPECT_LE(after.fragments.size(), settings.maxFragments);
    auto attachedArea = [](const auto& glass) {
        float area = 0;
        for (const auto& fragment : glass.fragments)
            if (fragment.attached)
                area += fragment.area;
        return area;
    };
    EXPECT_LT(attachedArea(after), attachedArea(before));
    for (std::size_t i = 0; i < before.fragments.size(); ++i)
        if (!before.fragments[i].attached)
            EXPECT_EQ(after.fragments[i].polygon, before.fragments[i].polygon);
    EXPECT_FALSE(scene.physics().raycast({found->center.x, found->center.y, 2}, {0, 0, -1}));
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
    editor.view<Engine::GlassDebrisComponent>([&](auto entity, const auto&) { debris.push_back(entity); });
    ASSERT_FALSE(debris.empty());
    EXPECT_LE(debris.size(), 3);
    const auto start = editor.read<Engine::Transform>(debris.front()).position;
    Engine::PhysicsSystem physics;
    for (int i = 0; i < 30; ++i)
        physics.update(scene, 1.0F / 60.0F);
    EXPECT_LT(editor.read<Engine::Transform>(debris.front()).position.y(), start.y());
    for (auto entity : debris) {
        EXPECT_GT(editor.read<Engine::RigidbodyComponent>(entity).mass, 0.0F);
        EXPECT_TRUE(editor.has<Engine::ColliderComponent>(entity));
        EXPECT_FALSE(editor.has<Engine::MeshRendererComponent>(entity));
        EXPECT_TRUE(editor.has<Engine::GlassFragmentBindingComponent>(entity));
    }
    Engine::GlassDestructionSystem::update(scene, 5.0F);
    for (auto entity : debris)
        EXPECT_FALSE(scene.valid(entity));
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
    for (int i = 0; i < 180; ++i)
        physics.update(scene, 1.0F / 60.0F);
    std::size_t count = 0;
    editor.view<Engine::GlassDebrisComponent, Engine::Transform>([&](auto, const auto&, const auto& transform) {
        ++count;
        EXPECT_GT(transform.position.y(), -0.05F);
        EXPECT_LT(transform.position.y(), 0.6F);
    });
    EXPECT_GT(count, 0);
    std::vector<glm::vec2> holes{{0.0F, 0.0F}};
    for (int iteration = 0; iteration < 8; ++iteration) {
        const auto glass = editor.read<Engine::GlassDestructionComponent>(entity);
        const auto fragment = std::ranges::find_if(glass.fragments, [](const auto& value) { return value.attached; });
        if (fragment == glass.fragments.end())
            break;
        holes.push_back(fragment->center);
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(
            scene, entity, {.worldPosition = {fragment->center.x, fragment->center.y + 2.0F, 0.0F}}));
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
        const auto bottom = std::ranges::find_if(
            glass.fragments, [](const auto& fragment) { return fragment.attached && fragment.frameSupported; });
        if (bottom == glass.fragments.end())
            break;
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(
            scene, entity, {.worldPosition = {bottom->center.x, bottom->center.y, 0.0F}, .radius = 0.03F}));
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
    restoredEditor.view<Engine::GlassDebrisComponent>([&](auto shard, const auto&) { debris.push_back(shard); });
    ASSERT_FALSE(debris.empty());
    const auto start = restoredEditor.read<Engine::Transform>(debris.front()).position.y();
    for (int i = 0; i < 20; ++i)
        restoredPhysics.update(restored, 1.0F / 60.0F);
    EXPECT_LT(restoredEditor.read<Engine::Transform>(debris.front()).position.y(), start);
}

TEST(GlassDestruction, RepeatedDamageRefinesPackedGeometryAndPreservesHoleCollision) {
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
    const auto next = std::ranges::find_if(glass.fragments, [](const auto& fragment) { return fragment.attached; });
    ASSERT_NE(next, glass.fragments.end());
    ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, entity,
                                                         {.worldPosition = {next->center.x, next->center.y, 0.0F}}));
    EXPECT_NE(editor.read<Engine::MeshRendererComponent>(entity).mesh.resource(), handle.resource());
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
    EXPECT_EQ(
        restoredEditor.read<Engine::MeshRendererComponent>(restoredEntity).mesh.resource()->metadata.sections.size(),
        editor.read<Engine::GlassDestructionComponent>(entity).fragments.size() + 1);
}

TEST(GlassDestruction, DebrisExpiresDuringPhysicsStepWithoutStaleActors) {
    Engine::Scene scene;
    Engine::GlassDestructionComponent settings;
    settings.fragmentLifetime = 0.05F;
    settings.maxDynamicFragments = 2;
    const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Pane", settings);
    ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, scene.findEntity(pane.id()), {}));
    Engine::PhysicsSystem physics;
    for (int i = 0; i < 12; ++i)
        ASSERT_NO_THROW(physics.update(scene, 1.0F / 60.0F));
    std::size_t count = 0;
    scene.editor().view<Engine::GlassDebrisComponent>([&](auto, const auto&) { ++count; });
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
    editor.view<Engine::GlassFragmentBindingComponent>([&](auto entity, const auto& binding) {
        shards.push_back(entity);
        EXPECT_EQ(binding.paneEntity, paneEntity);
        EXPECT_EQ(
            editor.read<Engine::GlassDestructionComponent>(paneEntity).fragments[binding.fragmentIndex].debrisEntity,
            entity);
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
    ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, entity, {.worldPosition = {-0.5F, 0, 0}, .energy = 2}));
    const auto hitMesh = editor.read<Engine::MeshRendererComponent>(entity).mesh;
    const auto firstFragmentCount = editor.read<Engine::GlassDestructionComponent>(entity).fragments.size();
    const auto hitTopology = editor.renderTopologyRevision();
    EXPECT_NE(hitMesh.resource(), mesh.resource());
    EXPECT_GT(hitTopology, topology);
    ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, entity, {.worldPosition = {0.5F, 0, 0}, .energy = 2}));
    const auto& glass = editor.read<Engine::GlassDestructionComponent>(entity);
    const auto refinedMesh = editor.read<Engine::MeshRendererComponent>(entity).mesh;
    EXPECT_FALSE(glass.fractured);
    EXPECT_EQ(glass.renderState, Engine::GlassRenderState::Cracked);
    ASSERT_EQ(glass.cracks.size(), 2);
    EXPECT_NE(glass.cracks[0].position, glass.cracks[1].position);
    EXPECT_NE(refinedMesh.resource(), hitMesh.resource());
    EXPECT_GT(editor.renderTopologyRevision(), hitTopology);
    EXPECT_GT(glass.fragments.size(), firstFragmentCount);
    EXPECT_TRUE(std::holds_alternative<Engine::BoxCollider>(editor.read<Engine::ColliderComponent>(entity).shape));
    EXPECT_TRUE(scene.physics().raycast({-0.5F, 0, 2}, {0, 0, -1}));
    std::ostringstream output;
    Engine::SceneSerializer::save(scene, output);
    Engine::Scene restored;
    std::istringstream input(output.str());
    Engine::SceneSerializer::load(restored, input);
    Engine::SceneEditor loaded(restored);
    const auto restoredEntity = restored.findEntity(restored.findActor("Pane").id());
    const auto& saved = loaded.read<Engine::GlassDestructionComponent>(restoredEntity);
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
    settings.size = {3.0F, 1.5F};
    settings.maxFragments = 128;
    const auto first = Engine::GlassDestructionSystem::createPane(scene, "First", settings);
    settings.thickness *= 2;
    const auto second = Engine::GlassDestructionSystem::createPane(scene, "Second", settings);
    Engine::SceneEditor editor(scene);
    const auto& a = editor.read<Engine::GlassDestructionComponent>(scene.findEntity(first.id()));
    const auto& b = editor.read<Engine::GlassDestructionComponent>(scene.findEntity(second.id()));
    float area = 0;
    for (const auto& fragment : a.fragments) {
        area += fragment.area;
        EXPECT_GT(fragment.area, 0);
        ASSERT_TRUE(fragment.collisionGeometry);
        for (const auto& part : fragment.collisionGeometry->convexCollisionParts)
            EXPECT_GE(part->vertices.size(), 6U);
    }
    EXPECT_NEAR(area, 4.5F, 0.0001F);
    EXPECT_LE(a.fragments.size(), settings.maxFragments);
    ASSERT_FALSE(a.bonds.empty());
    ASSERT_EQ(a.bonds.size(), b.bonds.size());
    for (std::size_t i = 0; i < a.bonds.size(); ++i) {
        const auto& bond = a.bonds[i];
        EXPECT_NEAR(bond.length, glm::distance(bond.edgeStart, bond.edgeEnd), 0.00001F);
        EXPECT_NEAR(b.bonds[i].strength, bond.strength * 2, 0.0001F);
    }
    const auto mesh = Engine::GlassDestructionSystem::renderMesh(a);
    std::size_t expectedVertices = 0;
    for (const auto& fragment : a.fragments)
        expectedVertices += fragment.polygon.size() * 6;
    EXPECT_EQ(mesh->vertices.size(), expectedVertices + 24);
}

TEST(GlassDestruction, RadialFractureStartsAtImpactAndConservesPaneAtEdges) {
    for (unsigned seed : {1U, 7U, 42U}) {
        for (glm::vec2 hit : {glm::vec2{0.35F, -0.2F}, glm::vec2{1.5F, 0}, glm::vec2{1.5F, 0.75F}}) {
            Engine::Scene scene;
            Engine::GlassDestructionComponent settings;
            settings.size = {3, 1.5F};
            settings.seed = seed;
            const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Radial", settings);
            const auto entity = scene.findEntity(pane.id());
            ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, entity,
                                                                 {.worldPosition = {hit.x, hit.y, 0}, .energy = 2}));
            Engine::SceneEditor editor(scene);
            const auto& glass = editor.read<Engine::GlassDestructionComponent>(entity);
            float area = 0;
            unsigned originEdges = 0;
            for (const auto& fragment : glass.fragments) {
                EXPECT_GT(fragment.area, 0);
                area += fragment.area;
                for (unsigned i = 0; i < fragment.polygon.size(); ++i) {
                    const auto a = fragment.polygon[i];
                    EXPECT_LE(std::abs(a.x), 1.50001F);
                    EXPECT_LE(std::abs(a.y), 0.75001F);
                }
            }
            for (const auto& bond : glass.bonds)
                originEdges +=
                    glm::distance(bond.edgeStart, hit) < 0.0001F || glm::distance(bond.edgeEnd, hit) < 0.0001F;
            EXPECT_NEAR(area, 4.5F, 0.0001F);
            EXPECT_LE(glass.fragments.size(), settings.maxFragments);
            EXPECT_GE(originEdges, 2U);
            for (const auto& vertex : Engine::GlassDestructionSystem::renderMesh(glass)->vertices)
                ASSERT_NEAR(glm::length(vertex.normal.native()), 1.0F, 0.0001F);
        }
    }
}

TEST(GlassDestruction, PrimarySpiderwebContainsContinuousCircumferentialFractureBands) {
    Engine::Scene scene;
    const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Spiderweb");
    const auto entity = scene.findEntity(pane.id());
    ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, entity, {.energy = 2, .radius = 0.4F}));
    Engine::SceneEditor editor(scene);
    const auto& glass = editor.read<Engine::GlassDestructionComponent>(entity);
    ASSERT_FALSE(glass.bonds.empty());
    auto cross = [](glm::vec2 a, glm::vec2 b) { return a.x * b.y - a.y * b.x; };
    // Every direction from a central impact crosses multiple complete
    // physical ring boundaries, rather than random neighboring cell edges.
    for (unsigned ray = 0; ray < 24; ++ray) {
        const float angle = (float(ray) + 0.317F) * glm::two_pi<float>() / 24.0F;
        const glm::vec2 direction{std::cos(angle), std::sin(angle)};
        std::vector<float> radii;
        for (const auto& bond : glass.bonds) {
            const auto edge = bond.edgeEnd - bond.edgeStart;
            const float denominator = cross(direction, edge);
            if (std::abs(denominator) < 1.0e-8F)
                continue;
            const float t = cross(bond.edgeStart, edge) / denominator;
            const float u = cross(bond.edgeStart, direction) / denominator;
            if (t < 0.02F || t > 0.9F || u < 0 || u > 1)
                continue;
            if (std::ranges::none_of(radii, [&](float value) { return std::abs(t - value) < 0.001F; }))
                radii.push_back(t);
        }
        EXPECT_GE(radii.size(), 4U);
    }
    const auto segments = Engine::GlassDestructionSystem::crackSegments(glass, glass.cracks.front());
    unsigned shared = 0;
    for (const auto& bond : glass.bonds) {
        if (glm::distance(bond.edgeStart, glass.cracks.front().position) > glass.cracks.front().radius ||
            glm::distance(bond.edgeEnd, glass.cracks.front().position) > glass.cracks.front().radius)
            continue;
        const auto visible = std::ranges::find_if(segments, [&](const auto& segment) {
            return glm::distance(segment.start, bond.edgeStart) < 1.0e-5F &&
                   glm::distance(segment.end, bond.edgeEnd) < 1.0e-5F && segment.intensity > 0;
        });
        EXPECT_NE(visible, segments.end());
        ++shared;
    }
    EXPECT_GT(shared, 100U);
    EXPECT_FALSE(glass.fractured);
    if (const char* path = std::getenv("GE_GLASS_PATTERN_CSV")) {
        std::ofstream output(path);
        output << "x1,y1,x2,y2,intensity\n";
        for (const auto& segment : segments)
            output << segment.start.x << ',' << segment.start.y << ',' << segment.end.x << ',' << segment.end.y << ','
                   << segment.intensity << '\n';
    }
}

TEST(GlassDestruction, RigidAndParticleDebrisShareOutwardImpulseDirections) {
    Engine::Scene rigidScene, particleScene;
    Engine::GlassDestructionComponent settings;
    settings.maxDynamicFragments = 128;
    rigidScene.glassSimulationSettings().maxNewDynamicFragmentsPerStep = 128;
    const auto rigid = Engine::GlassDestructionSystem::createPane(rigidScene, "Rigid", settings);
    settings.maxDynamicFragments = 0;
    const auto particles = Engine::GlassDestructionSystem::createPane(particleScene, "Particles", settings);
    const Engine::GlassImpact impact{.worldDirection = {0, 0, -1}, .energy = 80, .radius = 0.35F};
    ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(rigidScene, rigidScene.findEntity(rigid.id()), impact));
    ASSERT_TRUE(
        Engine::GlassDestructionSystem::onImpact(particleScene, particleScene.findEntity(particles.id()), impact));
    Engine::SceneEditor editor(rigidScene);
    unsigned compared = 0;
    editor.view<Engine::GlassDebrisComponent, Engine::Transform, Engine::RigidbodyState>(
        [&](auto, const auto&, const auto& pose, const auto& state) {
            const auto burst = std::ranges::find_if(particleScene.glassParticleBursts(), [&](const auto& value) {
                return glm::distance(value.position, pose.position.native()) < 0.0001F;
            });
            ASSERT_NE(burst, particleScene.glassParticleBursts().end());
            const auto velocity = state.linearVelocity.native();
            if (glm::length(velocity) < 0.001F)
                return;
            ++compared;
            EXPECT_NEAR(glm::dot(glm::normalize(velocity), glm::normalize(burst->velocity)), 1.0F, 0.0001F);
            EXPECT_LT(velocity.z, 0);
            EXPECT_GT(glm::dot(glm::vec2{velocity}, glm::vec2{pose.position.native()}), 0);
        });
    EXPECT_GT(compared, 3U);
}

TEST(GlassDestruction, PreparedPhysicsShapesToggleWithoutCookingOrReplacingPaneActor) {
    Engine::Scene scene;
    Engine::GlassDestructionComponent settings;
    settings.maxDynamicFragments = 0;
    const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Pane", settings);
    Engine::PhysicsSystem physics;
    physics.update(scene, 1.0F / 60);
    const auto before = physics.diagnostics();
    ASSERT_GT(before.convexCookCount, 0);
    ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, scene.findEntity(pane.id()), {}));
    physics.update(scene, 1.0F / 60);
    const auto after = physics.diagnostics();
    EXPECT_EQ(before.convexCookCount, after.convexCookCount);
    EXPECT_EQ(before.triangleCookCount, after.triangleCookCount);
    EXPECT_EQ(before.actorCreations, after.actorCreations);
    EXPECT_GT(after.glassShapeUpdates, before.glassShapeUpdates);
    EXPECT_FALSE(physics.raycast(scene, {0, 0, 2}, {0, 0, -1}));
}

TEST(GlassDestruction, PaneAndGlobalDebrisBudgetsAreIndependentAndEnergyIsBounded) {
    Engine::Scene scene;
    scene.glassSimulationSettings().maxDynamicFragmentsGlobal = 3;
    Engine::GlassDestructionComponent settings;
    settings.maxDynamicFragments = 2;
    const auto first = Engine::GlassDestructionSystem::createPane(scene, "First", settings);
    const auto second = Engine::GlassDestructionSystem::createPane(scene, "Second", settings);
    second.setPosition({4, 0, 0});
    ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, scene.findEntity(first.id()), {}));
    ASSERT_TRUE(
        Engine::GlassDestructionSystem::onImpact(scene, scene.findEntity(second.id()), {.worldPosition = {4, 0, 0}}));
    Engine::SceneEditor editor(scene);
    std::size_t count = 0, firstCount = 0, secondCount = 0;
    float kinetic = 0;
    editor.view<Engine::GlassDebrisComponent, Engine::GlassFragmentBindingComponent, Engine::RigidbodyState,
                Engine::RigidbodyComponent>(
        [&](auto, const auto&, const auto& binding, const auto& state, const auto& body) {
            ++count;
            firstCount += binding.paneEntity == scene.findEntity(first.id());
            secondCount += binding.paneEntity == scene.findEntity(second.id());
            const auto v = state.linearVelocity.native(), w = glm::radians(state.angularVelocity.native());
            EXPECT_TRUE(std::isfinite(glm::length(v)));
            EXPECT_TRUE(std::isfinite(glm::length(w)));
            EXPECT_LE(glm::length(v), scene.glassSimulationSettings().maxLinearSpeed + 0.001F);
            EXPECT_LE(glm::length(w), scene.glassSimulationSettings().maxAngularSpeed + 0.001F);
            kinetic += 0.5F * body.mass * glm::dot(v, v);
        });
    EXPECT_EQ(count, 3);
    EXPECT_EQ(firstCount, 2);
    EXPECT_EQ(secondCount, 1);
    EXPECT_LE(kinetic, 80.0F * scene.glassSimulationSettings().kineticEnergyFraction + 0.001F);
    EXPECT_FALSE(scene.glassParticleBursts().empty());
}

TEST(GlassDestruction, LaminatedAndArmoredGlassRetainWeakDamageWhileTemperedCrumbles) {
    Engine::Scene scene;
    Engine::SceneEditor editor(scene);
    Engine::GlassDestructionComponent settings;
    settings.maxDynamicFragments = 0;
    settings.type = Engine::GlassType::Laminated;
    const auto laminated = Engine::GlassDestructionSystem::createPane(scene, "Laminated", settings);
    const auto l = scene.findEntity(laminated.id());
    ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, l, {}));
    EXPECT_FALSE(editor.read<Engine::GlassDestructionComponent>(l).fractured);
    EXPECT_EQ(attachedCount(editor.read<Engine::GlassDestructionComponent>(l)),
              editor.read<Engine::GlassDestructionComponent>(l).fragments.size());
    settings.type = Engine::GlassType::Armored;
    settings.layers = 3;
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
    const auto base = scene.createActor("Base");
    base.setPosition({0, -0.1F, 0});
    base.addBoxCollider({2, 0.1F, 1});
    Engine::GlassDestructionComponent settings;
    settings.supportMode = Engine::GlassSupportMode::Bottom;
    const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Pane", settings);
    pane.setPosition({0, 1, 0});
    const auto entity = scene.findEntity(pane.id());
    const auto ball = scene.createActor("Ball");
    ball.setPosition({4, 3, 0});
    ball.addSphereCollider();
    ball.addRigidbody();
    Engine::PhysicsSystem physics;
    physics.update(scene, 1.0F / 60);
    Engine::SceneEditor editor(scene);
    const auto rays = editor.read<Engine::GlassDestructionComponent>(entity).supportRaycasts;
    EXPECT_EQ(rays, 3);
    for (unsigned i = 0; i < 10; ++i)
        physics.update(scene, 1.0F / 60);
    EXPECT_EQ(editor.read<Engine::GlassDestructionComponent>(entity).supportRaycasts, rays);
    scene.destroy(scene.findEntity(base.id()));
    physics.update(scene, 1.0F / 60);
    EXPECT_TRUE(editor.read<Engine::GlassDestructionComponent>(entity).fullyDestroyed);
}

TEST(GlassDestruction, BurstBudgetLimitsNewBodiesAndResetsAtTheNextSimulationStep) {
    Engine::Scene scene;
    scene.glassSimulationSettings().maxNewDynamicFragmentsPerStep = 2;
    scene.glassSimulationSettings().adaptivePhysicsBudget = false;
    Engine::GlassDestructionComponent settings;
    settings.maxDynamicFragments = 8;
    const auto first = Engine::GlassDestructionSystem::createPane(scene, "First", settings);
    const auto second = Engine::GlassDestructionSystem::createPane(scene, "Second", settings);
    const auto third = Engine::GlassDestructionSystem::createPane(scene, "Third", settings);
    second.setPosition({4, 0, 0});
    third.setPosition({8, 0, 0});
    ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, scene.findEntity(first.id()), {}));
    ASSERT_TRUE(
        Engine::GlassDestructionSystem::onImpact(scene, scene.findEntity(second.id()), {.worldPosition = {4, 0, 0}}));
    Engine::SceneEditor editor(scene);
    const auto countBodies = [&] {
        unsigned count = 0;
        editor.view<Engine::GlassDebrisComponent>([&](auto, const auto&) { ++count; });
        return count;
    };
    EXPECT_EQ(countBodies(), 2);
    EXPECT_FALSE(scene.glassParticleBursts().empty());
    EXPECT_FALSE(scene.physics().raycast({4, 0, 2}, {0, 0, -1}));
    Engine::GlassDestructionSystem::update(scene, 1.0F / 60);
    ASSERT_TRUE(
        Engine::GlassDestructionSystem::onImpact(scene, scene.findEntity(third.id()), {.worldPosition = {8, 0, 0}}));
    EXPECT_EQ(countBodies(), 4);
    EXPECT_FALSE(scene.physics().raycast({8, 0, 2}, {0, 0, -1}));
}

TEST(GlassDestruction, CrackHistoryStaysBoundedAndRetainsNewImpacts) {
    Engine::Scene scene;
    Engine::GlassDestructionComponent settings;
    settings.type = Engine::GlassType::Armored;
    settings.layers = 16;
    const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Pane", settings);
    const auto entity = scene.findEntity(pane.id());
    for (unsigned i = 0; i < 140; ++i)
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, entity, {.energy = 1}));
    Engine::SceneEditor editor(scene);
    const auto& glass = editor.read<Engine::GlassDestructionComponent>(entity);
    EXPECT_FALSE(glass.fractured);
    EXPECT_EQ(glass.impactCount, 140);
    EXPECT_EQ(glass.cracks.size(), 128);
    EXPECT_EQ(glass.cracks.back().seed, settings.seed + 140U * 1664525U);
}

TEST(GlassDestruction, TransformedPreparedShapesShareCookingWithDynamicShards) {
    Engine::Scene scene;
    Engine::GlassDestructionComponent settings;
    settings.maxDynamicFragments = 3;
    const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Pane", settings);
    pane.setPosition({3, 4, 5});
    pane.setRotation({31, 27, 13});
    pane.setScale({1.3F, 1.7F, 0.9F});
    Engine::PhysicsSystem physics;
    physics.update(scene, 1.0F / 60);
    const auto before = physics.diagnostics();
    ASSERT_TRUE(
        Engine::GlassDestructionSystem::onImpact(scene, scene.findEntity(pane.id()), {.worldPosition = {3, 4, 5}}));
    physics.update(scene, 1.0F / 60);
    EXPECT_EQ(physics.diagnostics().convexCookCount, before.convexCookCount);
}

TEST(GlassDestruction, IdenticalPanesShareGeometryAndCookingButKeepIndependentDamage) {
    Engine::Scene scene;
    Engine::SceneEditor editor(scene);
    Engine::GlassDestructionComponent settings;
    settings.maxDynamicFragments = 0;
    const auto first = Engine::GlassDestructionSystem::createPane(scene, "First", settings);
    const auto second = Engine::GlassDestructionSystem::createPane(scene, "Second", settings);
    second.setPosition({4, 0, 0});
    const auto a = scene.findEntity(first.id()), b = scene.findEntity(second.id());
    EXPECT_EQ(editor.read<Engine::MeshRendererComponent>(a).mesh.resource(),
              editor.read<Engine::MeshRendererComponent>(b).mesh.resource());
    EXPECT_EQ(editor.read<Engine::GlassDestructionComponent>(a).fragments.front().collisionGeometry,
              editor.read<Engine::GlassDestructionComponent>(b).fragments.front().collisionGeometry);
    Engine::PhysicsSystem physics;
    physics.update(scene, 1.0F / 60);
    std::size_t expectedCooks = 0;
    for (const auto& fragment : editor.read<Engine::GlassDestructionComponent>(a).fragments)
        expectedCooks += std::max(std::size_t{1}, fragment.collisionGeometry->convexCollisionParts.size());
    EXPECT_EQ(physics.diagnostics().convexCookCount, expectedCooks);
    ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, a, {}));
    EXPECT_FALSE(editor.read<Engine::GlassDestructionComponent>(b).fractured);
    EXPECT_TRUE(editor.read<Engine::GlassDestructionComponent>(b).cracks.empty());
    ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, b, {.worldPosition = {4, 0, 0}}));
    EXPECT_EQ(editor.read<Engine::MeshRendererComponent>(a).mesh.resource(),
              editor.read<Engine::MeshRendererComponent>(b).mesh.resource());
}

TEST(GlassDestruction, SharedGraphEdgesControlCrackMaskAndSideExposure) {
    Engine::Scene scene;
    const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Graph");
    Engine::SceneEditor editor(scene);
    auto glass = editor.read<Engine::GlassDestructionComponent>(scene.findEntity(pane.id()));
    ASSERT_FALSE(glass.bonds.empty());
    const auto bond = glass.bonds.front();
    auto edgeIndex = [&](unsigned index) {
        const auto& polygon = glass.fragments[index].polygon;
        for (unsigned edge = 0; edge < polygon.size(); ++edge) {
            const auto a = polygon[edge], b = polygon[(edge + 1) % polygon.size()];
            if ((glm::distance(a, bond.edgeStart) < 0.0001F && glm::distance(b, bond.edgeEnd) < 0.0001F) ||
                (glm::distance(b, bond.edgeStart) < 0.0001F && glm::distance(a, bond.edgeEnd) < 0.0001F))
                return edge;
        }
        return ~0U;
    };
    const auto first = edgeIndex(bond.first), second = edgeIndex(bond.second);
    ASSERT_NE(first, ~0U);
    ASSERT_NE(second, ~0U);
    EXPECT_FALSE(Engine::GlassDestructionSystem::edgeExposed(glass, bond.first, first));
    EXPECT_FALSE(Engine::GlassDestructionSystem::edgeExposed(glass, bond.second, second));
    glass.bonds.front().broken = true;
    EXPECT_TRUE(Engine::GlassDestructionSystem::edgeExposed(glass, bond.first, first));
    EXPECT_TRUE(Engine::GlassDestructionSystem::edgeExposed(glass, bond.second, second));
    glass.bonds.front().broken = false;
    glass.fragments[bond.second].attached = false;
    EXPECT_TRUE(Engine::GlassDestructionSystem::edgeExposed(glass, bond.first, first));
    glass.bonds.front().broken = true;
    // Broken edges must appear even outside the impact footprint.
    const auto segments = Engine::GlassDestructionSystem::crackSegments(glass, {{0.8F, 0.8F}, 0.1F, 0.4F, 7});
    EXPECT_TRUE(std::ranges::any_of(segments, [&](const auto& segment) {
        return segment.start == bond.edgeStart && segment.end == bond.edgeEnd && segment.intensity == 1.0F;
    }));
}

TEST(GlassDestruction, OffCenterGraphAndSurfaceTipsSurviveSerialization) {
    Engine::Scene scene;
    const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Offset");
    const auto entity = scene.findEntity(pane.id());
    Engine::PhysicsSystem physics;
    physics.update(scene, 1.0F / 60);
    const auto initialCookCount = physics.diagnostics().convexCookCount;
    ASSERT_TRUE(
        Engine::GlassDestructionSystem::onImpact(scene, entity, {.worldPosition = {0.65F, -0.45F, 0}, .energy = 2}));
    physics.update(scene, 1.0F / 60);
    EXPECT_GT(physics.diagnostics().convexCookCount, initialCookCount);
    const auto hitCookCount = physics.diagnostics().convexCookCount;
    physics.update(scene, 1.0F / 60);
    EXPECT_EQ(physics.diagnostics().convexCookCount, hitCookCount);
    Engine::SceneEditor editor(scene);
    const auto glass = editor.read<Engine::GlassDestructionComponent>(entity);
    const auto segments = Engine::GlassDestructionSystem::crackSegments(glass, glass.cracks.front());
    ASSERT_FALSE(segments.empty());
    bool curved = false;
    for (const auto& segment : segments) {
        EXPECT_LE(glm::abs(segment.start).x, glass.size.x * 0.5F + 0.0001F);
        EXPECT_LE(glm::abs(segment.start).y, glass.size.y * 0.5F + 0.0001F);
        EXPECT_LE(glm::abs(segment.end).x, glass.size.x * 0.5F + 0.0001F);
        EXPECT_LE(glm::abs(segment.end).y, glass.size.y * 0.5F + 0.0001F);
        for (const auto& next : segments)
            if (segment.end == next.start) {
                const auto a = segment.end - segment.start, b = next.end - next.start;
                curved |= std::abs(a.x * b.y - a.y * b.x) > 1.0e-8F;
            }
    }
    EXPECT_TRUE(curved);
    std::ostringstream output;
    Engine::SceneSerializer::save(scene, output);
    Engine::Scene restored;
    std::istringstream input(output.str());
    Engine::SceneSerializer::load(restored, input);
    Engine::SceneEditor loaded(restored);
    const auto& saved =
        loaded.read<Engine::GlassDestructionComponent>(restored.findEntity(restored.findActor("Offset").id()));
    ASSERT_EQ(saved.fragments.size(), glass.fragments.size());
    for (unsigned i = 0; i < glass.fragments.size(); ++i)
        EXPECT_EQ(saved.fragments[i].polygon, glass.fragments[i].polygon);
    const auto replay = Engine::GlassDestructionSystem::crackSegments(saved, saved.cracks.front());
    ASSERT_EQ(replay.size(), segments.size());
    for (unsigned i = 0; i < segments.size(); ++i) {
        EXPECT_EQ(replay[i].start, segments[i].start);
        EXPECT_EQ(replay[i].end, segments[i].end);
        EXPECT_EQ(replay[i].intensity, segments[i].intensity);
    }
}

TEST(GlassDestruction, NewImpactPreservesLiveShardBindingsAndConservesArea) {
    Engine::Scene scene;
    Engine::GlassDestructionComponent settings;
    settings.maxDynamicFragments = 4;
    const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Multi impact", settings);
    const auto entity = scene.findEntity(pane.id());
    Engine::SceneEditor editor(scene);
    ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, entity, {}));
    const auto before = editor.read<Engine::GlassDestructionComponent>(entity);
    const auto target = std::ranges::max_element(
        before.fragments, {}, [](const auto& fragment) { return fragment.attached ? fragment.area : 0.0F; });
    ASSERT_TRUE(target->attached);
    ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, entity,
                                                         {.worldPosition = {target->center.x, target->center.y, 0}}));
    const auto& after = editor.read<Engine::GlassDestructionComponent>(entity);
    EXPECT_GT(after.fragments.size(), before.fragments.size());
    EXPECT_LE(after.fragments.size(), settings.maxFragments);
    float area = 0;
    unsigned liveShards = 0;
    for (const auto& fragment : after.fragments) {
        area += fragment.area;
        EXPECT_GT(fragment.area, 0);
        ASSERT_TRUE(fragment.collisionGeometry);
    }
    EXPECT_NEAR(area, settings.size.x * settings.size.y, 0.0001F);
    for (unsigned i = 0; i < before.fragments.size(); ++i) {
        if (before.fragments[i].attached)
            continue;
        EXPECT_FALSE(after.fragments[i].attached);
        EXPECT_EQ(after.fragments[i].polygon, before.fragments[i].polygon);
        EXPECT_EQ(after.fragments[i].center, before.fragments[i].center);
        EXPECT_EQ(after.fragments[i].collisionGeometry, before.fragments[i].collisionGeometry);
        EXPECT_EQ(after.fragments[i].debrisEntity, before.fragments[i].debrisEntity);
        if (scene.valid(before.fragments[i].debrisEntity)) {
            ++liveShards;
            EXPECT_EQ(
                editor.read<Engine::GlassFragmentBindingComponent>(before.fragments[i].debrisEntity).fragmentIndex, i);
        }
    }
    EXPECT_GT(liveShards, 0U);
    for (const auto& bond : after.bonds) {
        EXPECT_LT(bond.first, after.fragments.size());
        EXPECT_LT(bond.second, after.fragments.size());
        EXPECT_GT(bond.length, 0);
    }
}

TEST(GlassDestruction, WeakShotsRevealPrimarySpiderwebAndIndependentSecondaryCracks) {
    Engine::Scene scene;
    const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Surface cracks");
    const auto entity = scene.findEntity(pane.id());
    for (float x : {-0.65F, 0.65F})
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(
            scene, entity, {.worldPosition = {x, 0, 0}, .energy = 2, .radius = 0.1F}));
    Engine::SceneEditor editor(scene);
    const auto& glass = editor.read<Engine::GlassDestructionComponent>(entity);
    ASSERT_EQ(glass.cracks.size(), 2);
    for (const auto& crack : glass.cracks) {
        const auto segments = Engine::GlassDestructionSystem::crackSegments(glass, crack);
        ASSERT_FALSE(segments.empty());
        unsigned origins = 0;
        float reach = 0;
        for (const auto& segment : segments) {
            origins += glm::distance(segment.start, crack.position) < 0.00001F ||
                       glm::distance(segment.end, crack.position) < 0.00001F;
            reach = std::max(reach, glm::distance(segment.end, crack.position));
            if (crack.position != glass.cracks.front().position)
                for (const auto& bond : glass.bonds)
                    EXPECT_FALSE(segment.start == bond.edgeStart && segment.end == bond.edgeEnd);
        }
        EXPECT_GE(origins, 7U);
        EXPECT_GT(reach, crack.radius * 0.2F);
    }
    EXPECT_FALSE(glass.fractured);
}

} // namespace
