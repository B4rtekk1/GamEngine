#include <gtest/gtest.h>

#include "Engine/ECS/Components/ColliderComponent.h"
#include "Engine/ECS/Components/ColorPickerComponent.h"
#include "Engine/ECS/Components/MeshRendererComponent.h"
#include "Engine/ECS/Components/RigidbodyComponent.h"
#include "Engine/ECS/Components/ScriptComponent.h"
#include "Engine/ECS/Components/TerrainComponent.h"
#include "Engine/ECS/Components/TerrainGrassComponent.h"
#include "Engine/ECS/Components/ProceduralCloudComponent.h"
#include "Engine/Renderer/Geometry/ProceduralCloud.h"
#include "Engine/Renderer/Lighting/DirectionalLightData.h"
#include "Engine/Renderer/Materials/PBRMaterial.h"
#include "Engine/Renderer/Materials/MaterialBuffer.h"
#include "Engine/Renderer/RenderConfig.h"
#include "Engine/Renderer/Particles/ParticleSystem.h"
#include "Engine/Scene/Components/IdentityComponents.h"
#include "Engine/Scene/Components/LightComponent.h"
#include "Engine/UI/Components/TextComponent.h"
#include "Engine/UI/UIVertex.h"

#include <limits>
#include <memory>
#include <type_traits>
#include <variant>

namespace {

void ExpectVec3Near(const Engine::Vec3& value, float x, float y, float z) {
    EXPECT_NEAR(value.x(), x, 1.0e-5F);
    EXPECT_NEAR(value.y(), y, 1.0e-5F);
    EXPECT_NEAR(value.z(), z, 1.0e-5F);
}

TEST(RigidbodyComponent, AccumulatesAndClearsForcesAndImpulses) {
    Engine::RigidbodyComponent body;
    body.mass = 2.0F;
    body.addForce({1.0F, 2.0F, 3.0F});
    body.addForce({-1.0F, 4.0F, -2.0F});
    body.addTorque({5.0F, 6.0F, 7.0F});
    body.addAngularImpulse({2.0F, 3.0F, 4.0F});
    body.addImpulse({4.0F, -2.0F, 6.0F});
    ExpectVec3Near(body.accumulatedForce(), 0.0F, 6.0F, 1.0F);
    ExpectVec3Near(body.accumulatedTorque(), 5.0F, 6.0F, 7.0F);
    ExpectVec3Near(body.accumulatedAngularImpulse(), 2.0F, 3.0F, 4.0F);
    ExpectVec3Near(body.linearVelocity, 2.0F, -1.0F, 3.0F);
    body.zeroForces();
    ExpectVec3Near(body.accumulatedForce(), 0.0F, 0.0F, 0.0F);
    ExpectVec3Near(body.accumulatedTorque(), 0.0F, 0.0F, 0.0F);
    ExpectVec3Near(body.accumulatedAngularImpulse(), 0.0F, 0.0F, 0.0F);
}

TEST(ProceduralCloud, ProducesDeterministicThreeDimensionalMesh) {
    Engine::ProceduralCloudComponent settings;
    settings.seed = 42U;
    settings.puffCount = 8U;
    settings.dimensions = {10.0F, 3.0F, 6.0F};
    const Engine::Mesh first = Engine::ProceduralCloud::createMesh(settings);
    const Engine::Mesh second = Engine::ProceduralCloud::createMesh(settings);

    ASSERT_FALSE(first.empty());
    EXPECT_EQ(first.vertices.size(), second.vertices.size());
    EXPECT_EQ(first.indices, second.indices);
    EXPECT_FLOAT_EQ(first.vertices.front().position.x(), second.vertices.front().position.x());
    EXPECT_FLOAT_EQ(first.vertices.front().position.y(), second.vertices.front().position.y());
    EXPECT_FLOAT_EQ(first.vertices.front().position.z(), second.vertices.front().position.z());
}

TEST(RigidbodyComponent, IgnoresLinearImpulseForNonPositiveMassAndStops) {
    Engine::RigidbodyComponent body;
    body.mass = 0.0F;
    body.addImpulse({1.0F, 2.0F, 3.0F});
    ExpectVec3Near(body.linearVelocity, 0.0F, 0.0F, 0.0F);
    body.linearVelocity = {1.0F, 2.0F, 3.0F};
    body.angularVelocity = {4.0F, 5.0F, 6.0F};
    body.addAngularImpulse({7.0F, 8.0F, 9.0F});
    body.stop();
    ExpectVec3Near(body.linearVelocity, 0.0F, 0.0F, 0.0F);
    ExpectVec3Near(body.angularVelocity, 0.0F, 0.0F, 0.0F);
    ExpectVec3Near(body.accumulatedAngularImpulse(), 0.0F, 0.0F, 0.0F);
}

TEST(ColliderComponent, DefaultsToBoxAndSupportsAllValueShapes) {
    Engine::ColliderComponent collider;
    ASSERT_TRUE(std::holds_alternative<Engine::BoxCollider>(collider.shape));
    const auto& box = std::get<Engine::BoxCollider>(collider.shape);
    ExpectVec3Near(box.halfExtents, 0.5F, 0.5F, 0.5F);
    EXPECT_FALSE(collider.isTrigger);
    EXPECT_FLOAT_EQ(collider.friction, 0.5F);
    collider.shape = Engine::SphereCollider{2.0F};
    EXPECT_FLOAT_EQ(std::get<Engine::SphereCollider>(collider.shape).radius, 2.0F);
    collider.shape = Engine::CapsuleCollider{0.5F, 3.0F};
    EXPECT_FLOAT_EQ(std::get<Engine::CapsuleCollider>(collider.shape).height, 3.0F);
    collider.shape = Engine::RampCollider{{1.0F, 2.0F, 3.0F}};
    ExpectVec3Near(std::get<Engine::RampCollider>(collider.shape).halfExtents, 1.0F, 2.0F, 3.0F);
}

TEST(SceneComponents, ProvideExpectedDefaults) {
    const Engine::LightComponent light;
    EXPECT_EQ(light.type, Engine::LightType::Directional);
    EXPECT_FLOAT_EQ(light.color.r(), 1.0F);
    EXPECT_TRUE(light.enabled);
    EXPECT_TRUE(light.castShadows);
    const Engine::ColorPickerComponent picker;
    EXPECT_FLOAT_EQ(picker.color.a(), 1.0F);
    const Engine::PBRMaterial material;
    EXPECT_FLOAT_EQ(material.roughness, 0.55F);
    EXPECT_EQ(material.baseColorTexture, -1);
    EXPECT_FALSE(material.alphaBlend);
}

TEST(IdentityComponents, GenerateAndReserveMonotonicUniqueIdentifiers) {
    const auto first = Engine::createUUID();
    const auto second = Engine::createUUID();
    EXPECT_NE(first, Engine::NullUUID);
    EXPECT_GT(second, first);
    Engine::reserveUUID(second + 100);
    EXPECT_GT(Engine::createUUID(), second + 100);
    const Engine::NameComponent name;
    EXPECT_EQ(name.value, "GameObject");
    EXPECT_EQ(Engine::ParentComponent{}.parentUuid, Engine::NullUUID);
}

TEST(TextComponent, IsRenderableOnlyWithVisibleNonEmptyPositiveLayout) {
    Engine::TextComponent text;
    EXPECT_FALSE(text.isRenderable());
    text.text = "Hello";
    EXPECT_TRUE(text.isRenderable());
    text.visible = false;
    EXPECT_FALSE(text.isRenderable());
    text.visible = true;
    text.fontSize = 0.0F;
    EXPECT_FALSE(text.isRenderable());
    text.fontSize = 16.0F;
    text.horizontalScale = -1.0F;
    EXPECT_FALSE(text.isRenderable());
}

TEST(RenderTypes, ExposeStableDefaultsAndHandles) {
    const Engine::RenderConfig config;
    EXPECT_TRUE(config.features.instancedRendering);
    EXPECT_TRUE(config.features.gpuCulling);
    EXPECT_FALSE(config.features.shadows);
    EXPECT_EQ(config.antialiasing, Engine::AntialiasingLevel::Off);
    EXPECT_FALSE(Engine::ViewportHandle{});
    EXPECT_TRUE((Engine::ViewportHandle{123}));
    const Engine::EditorEventState events;
    EXPECT_FALSE(events.quitRequested);
    EXPECT_FALSE(events.togglePlay);
}

TEST(RenderDataLayouts, MatchExpectedGpuFriendlySizes) {
    EXPECT_EQ(Engine::UI::UIVertex::size(), sizeof(Engine::UI::UIVertex));
    EXPECT_EQ(sizeof(Engine::DirectionalLightGPU), 32u);
    EXPECT_TRUE((std::is_standard_layout_v<Engine::DirectionalLightGPU>));
}

TEST(MeshRendererComponent, RequiresNonEmptySharedMeshToBeRenderable) {
    Engine::MeshRendererComponent renderer;
    EXPECT_FALSE(renderer.hasMesh());
    renderer.mesh = std::make_shared<const Engine::Mesh>();
    EXPECT_FALSE(renderer.hasMesh());
    auto populatedMesh = std::make_shared<Engine::Mesh>();
    populatedMesh->vertices.resize(3);
    populatedMesh->indices = {0, 1, 2};
    renderer.mesh = populatedMesh;
    EXPECT_TRUE(renderer.hasMesh());
    EXPECT_TRUE(renderer.castShadow);
    EXPECT_EQ(renderer.cullingBatch, 0u);
    EXPECT_EQ(renderer.occlusionQueryIndex, std::numeric_limits<std::uint32_t>::max());
}

TEST(TerrainComponent, BuildsCheckerboardGridAndSculptsHeightmap) {
    Engine::TerrainComponent terrain{5, 4.0F, 4.0F, -2.0F, 2.0F};
    ASSERT_TRUE(terrain.valid());
    const Engine::Mesh flatMesh = terrain.createMesh();
    EXPECT_EQ(flatMesh.vertices.size(), 25u);
    EXPECT_EQ(flatMesh.indices.size(), 96u);
    EXPECT_LT(flatMesh.vertices[0].color.x(), 0.0F);
    EXPECT_FLOAT_EQ(flatMesh.vertices[0].color.y(), 4.0F);

    EXPECT_TRUE(terrain.sculpt(0.0F, 0.0F, 1.5F, 0.5F,
                               Engine::TerrainSculptMode::Raise));
    EXPECT_GT(terrain.height(2, 2), 0.0F);
    EXPECT_FLOAT_EQ(terrain.height(0, 0), 0.0F);
    Engine::Mesh sculptedMesh = flatMesh;
    Engine::TerrainRegion center{2, 2, 2, 2, true};
    EXPECT_TRUE(terrain.updateMeshRegion(sculptedMesh, center));
    EXPECT_GT(sculptedMesh.vertices[12].position.y(), 0.0F);
    EXPECT_GT(sculptedMesh.vertices[12].normal.length(), 0.99F);

    const Engine::Mesh lodMesh = terrain.createMesh(1);
    EXPECT_EQ(lodMesh.vertices.size(), 9u);
    EXPECT_EQ(lodMesh.indices.size(), 24u);
}

TEST(TerrainGrassComponent, StoresCompactInstancesWithoutEntities) {
    Engine::TerrainGrassComponent grass;
    auto mesh = std::make_shared<Engine::Mesh>();
    mesh->vertices.resize(3);
    mesh->indices = {0, 1, 2};
    grass.mesh = mesh;
    grass.castShadow = false;
    grass.instances = {
        {.position = {1.0F, 0.25F, -1.0F}, .yaw = 42.0F, .scale = 0.8F},
        {.position = {-1.0F, 0.0F, 1.0F}, .yaw = 180.0F, .scale = 1.2F},
    };
    ASSERT_TRUE(grass.hasPrefab());
    ASSERT_EQ(grass.instances.size(), 2u);
    EXPECT_FLOAT_EQ(grass.instances[0].position.x(), 1.0F);
    EXPECT_FLOAT_EQ(grass.instances[0].yaw, 42.0F);
    EXPECT_FLOAT_EQ(grass.instances[1].scale, 1.2F);
    EXPECT_FALSE(grass.castShadow);
}

TEST(ScriptComponent, CopiesConfigurationWithoutSharingRuntimeState) {
    Engine::ScriptComponent script{"PlayerController", false};
    Engine::ScriptComponent copy{script};
    EXPECT_EQ(copy.className, "PlayerController");
    EXPECT_FALSE(copy.enabled);
    copy.className = "CameraController";
    copy.enabled = true;
    script = copy;
    EXPECT_EQ(script.className, "CameraController");
    EXPECT_TRUE(script.enabled);
    script.reset();
    EXPECT_EQ(script.className, "CameraController");
}

TEST(RenderTypes, AllowFeatureAndEventConfiguration) {
    Engine::RenderConfig config;
    config.features.shadows = true;
    config.features.occlusionCulling = true;
    config.antialiasing = Engine::AntialiasingLevel::MSAA4x;
    EXPECT_TRUE(config.features.shadows);
    EXPECT_TRUE(config.features.occlusionCulling);
    EXPECT_EQ(config.antialiasing, Engine::AntialiasingLevel::MSAA4x);
    Engine::EditorEventState events{.quitRequested = true, .togglePlay = true};
    EXPECT_TRUE(events.quitRequested);
    EXPECT_TRUE(events.togglePlay);
    EXPECT_FALSE(events.togglePause);
}

TEST(ParticleTypes, SmokeEmitterProvidesStableSimulationDefaults) {
    const Engine::Particles::ParticleEmitter emitter;
    EXPECT_FLOAT_EQ(emitter.spawnRate, 200.0F);
    EXPECT_FLOAT_EQ(emitter.minLifeTime, 1.0F);
    EXPECT_FLOAT_EQ(emitter.maxLifeTime, 2.0F);
    EXPECT_FLOAT_EQ(emitter.accumulator, 0.0F);

    const Engine::Particles::SmokeEmitter smoke;
    EXPECT_FLOAT_EQ(smoke.buoyancy, Engine::Particles::DefaultSmokeBuoyancy);
    EXPECT_FLOAT_EQ(smoke.drag, 0.68F);
    EXPECT_FLOAT_EQ(smoke.turbulence, 0.30F);
    EXPECT_FLOAT_EQ(smoke.collisionRadius, 0.10F);
    EXPECT_FLOAT_EQ(smoke.minVelocity.y(), 0.45F);
    EXPECT_FLOAT_EQ(smoke.maxVelocity.y(), 1.05F);
    EXPECT_FLOAT_EQ(smoke.minLifeTime, 5.5F);
    EXPECT_FLOAT_EQ(smoke.maxLifeTime, 9.0F);
    EXPECT_FLOAT_EQ(smoke.spawnRate, 260.0F);
    EXPECT_LT(smoke.color.a(), 0.2F);

    const Engine::Particles::ParticleCollider collider{
        .center = {1.0F, 2.0F, 3.0F, 0.0F},
        .halfExtents = {4.0F, 5.0F, 6.0F, 0.0F},
    };
    EXPECT_FLOAT_EQ(collider.center.z(), 3.0F);
    EXPECT_FLOAT_EQ(collider.halfExtents.y(), 5.0F);
}

TEST(MaterialTypes, ExposeGpuFriendlyDefaultsAndLayerConfiguration) {
    EXPECT_EQ(Engine::MaxMaterialTextures, 16u);
    EXPECT_EQ(alignof(Engine::GPUMaterialData), 16u);
    EXPECT_EQ(sizeof(Engine::GPUMaterialData), 64u);
    const Engine::GPUMaterialData gpuMaterial;
    EXPECT_EQ(gpuMaterial.textureIndices[0], -1);
    EXPECT_EQ(gpuMaterial.textureIndices[3], -1);
    EXPECT_EQ(gpuMaterial.terrainLayerTextures[0], -1);
    EXPECT_EQ(gpuMaterial.terrainLayerTextures[3], -1);

    Engine::PBRMaterial material;
    material.doubleSided = true;
    material.terrainLayered = true;
    material.terrainLayerTextures = {2, 3, 5, 7};
    material.normalScale = 0.0F;
    EXPECT_TRUE(material.doubleSided);
    EXPECT_TRUE(material.terrainLayered);
    EXPECT_EQ(material.terrainLayerTextures[2], 5);
    EXPECT_FLOAT_EQ(material.normalScale, 0.0F);
}

} // namespace
