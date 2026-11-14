#include "Engine/ECS/Components/ColliderComponent.h"
#include "Engine/ECS/Components/MeshRendererComponent.h"
#include "Engine/ECS/Components/ProceduralCloudComponent.h"
#include "Engine/ECS/Components/ReflectionProbeComponent.h"
#include "Engine/ECS/Components/RigidbodyComponent.h"
#include "Engine/ECS/Components/RigidbodyRuntime.h"
#include "Engine/ECS/Components/ScriptComponent.h"
#include "Engine/ECS/Components/TerrainComponent.h"
#include "Engine/ECS/Components/TerrainGrassComponent.h"
#include "Engine/Renderer/Geometry/GpuVertex.h"
#include "Engine/Renderer/Geometry/ProceduralCloud.h"
#include "Engine/Renderer/Lighting/DirectionalLightData.h"
#include "Engine/Renderer/Lighting/EnvironmentBaker.h"
#include "Engine/Renderer/Lighting/LightProfiles.h"
#include "Engine/Renderer/Lighting/ReflectionProbeCapture.h"
#include "Engine/Renderer/Materials/MaterialBuffer.h"
#include "Engine/Renderer/Materials/PBRMaterial.h"
#include "Engine/Renderer/Particles/ParticleSystem.h"
#include "Engine/Renderer/RenderConfig.h"
#include "Engine/Scene/Components/IdentityComponents.h"
#include "Engine/Scene/Components/LightComponent.h"
#include "Engine/UI/Components/TextComponent.h"
#include "Engine/UI/UIVertex.h"

#include <algorithm>
#include <cmath>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>
#include <vector>
#include <cstddef>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <type_traits>
#include <variant>

namespace {

TEST(ReflectionProbeComponent, ComputesBoxAndSphereInfluence) {
    Engine::ReflectionProbeComponent probe;
    probe.extents = {4.0F, 3.0F, 2.0F};
    probe.blendDistance = 1.0F;
    EXPECT_FLOAT_EQ(probe.influence({0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F}), 1.0F);
    EXPECT_FLOAT_EQ(probe.influence({0.0F, 0.0F, 0.0F}, {3.5F, 0.0F, 0.0F}), 0.5F);
    EXPECT_FLOAT_EQ(probe.influence({0.0F, 0.0F, 0.0F}, {4.1F, 0.0F, 0.0F}), 0.0F);

    probe.shape = Engine::ReflectionProbeShape::Sphere;
    probe.extents = {5.0F, 0.0F, 0.0F};
    EXPECT_FLOAT_EQ(probe.influence({0.0F, 0.0F, 0.0F}, {4.5F, 0.0F, 0.0F}), 0.5F);
}

TEST(ReflectionProbeCapture, UsesCanonicalCubemapCamerasAndFullMipChain) {
    constexpr auto faces = Engine::ReflectionProbeCapture::faces();
    EXPECT_FLOAT_EQ(faces[0].direction.x(), 1.0F);
    EXPECT_FLOAT_EQ(faces[0].up.y(), -1.0F);
    EXPECT_FLOAT_EQ(faces[2].direction.y(), 1.0F);
    EXPECT_FLOAT_EQ(faces[2].up.z(), 1.0F);
    EXPECT_FLOAT_EQ(faces[3].up.z(), -1.0F);
    EXPECT_EQ(Engine::ReflectionProbeCapture::fullMipCount(256), 9U);
    EXPECT_EQ(Engine::ReflectionProbeCapture::fullMipCount(1), 1U);
    EXPECT_EQ(Engine::ReflectionProbeCapture::fullMipCount(0), 0U);
}

TEST(EnvironmentBaker, MapsCompleteCubemapMipChainToRoughness) {
    EXPECT_FLOAT_EQ(Engine::EnvironmentBaker::roughnessForMip(0, 9), 0.0F);
    EXPECT_FLOAT_EQ(Engine::EnvironmentBaker::roughnessForMip(1, 9), 0.125F);
    EXPECT_FLOAT_EQ(Engine::EnvironmentBaker::roughnessForMip(8, 9), 1.0F);
    EXPECT_FLOAT_EQ(Engine::EnvironmentBaker::roughnessForMip(0, 1), 0.0F);
}

TEST(EnvironmentBaker, DiffuseIrradianceFoldsInLambertianNormalization) {
    constexpr Engine::Vec3 Radiance{0.25F, 0.5F, 0.75F};

    const Engine::Vec3 baked =
        Engine::EnvironmentBaker::diffuseIrradiance({0.0F, 1.0F, 0.0F}, [](const Engine::Vec3&) { return Radiance; });

    EXPECT_FLOAT_EQ(baked.x(), Radiance.x());
    EXPECT_FLOAT_EQ(baked.y(), Radiance.y());
    EXPECT_FLOAT_EQ(baked.z(), Radiance.z());
}

void ExpectVec3Near(const Engine::Vec3& value, float x, float y, float z) {
    EXPECT_NEAR(value.x(), x, 1.0e-5F);
    EXPECT_NEAR(value.y(), y, 1.0e-5F);
    EXPECT_NEAR(value.z(), z, 1.0e-5F);
}

TEST(PhysicsCommandBuffer, AccumulatesAndClearsForcesAndImpulses) {
    Engine::PhysicsCommandBuffer commands;
    commands.force += Engine::Vec3{1.0F, 2.0F, 3.0F};
    commands.force += Engine::Vec3{-1.0F, 4.0F, -2.0F};
    commands.torque = {5.0F, 6.0F, 7.0F};
    commands.angularImpulse = {2.0F, 3.0F, 4.0F};
    commands.impulse = {4.0F, -2.0F, 6.0F};
    ExpectVec3Near(commands.force, 0.0F, 6.0F, 1.0F);
    ExpectVec3Near(commands.torque, 5.0F, 6.0F, 7.0F);
    ExpectVec3Near(commands.angularImpulse, 2.0F, 3.0F, 4.0F);
    ExpectVec3Near(commands.impulse, 4.0F, -2.0F, 6.0F);
    commands.clear();
    ExpectVec3Near(commands.force, 0.0F, 0.0F, 0.0F);
    ExpectVec3Near(commands.torque, 0.0F, 0.0F, 0.0F);
    ExpectVec3Near(commands.angularImpulse, 0.0F, 0.0F, 0.0F);
    ExpectVec3Near(commands.impulse, 0.0F, 0.0F, 0.0F);
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

TEST(GpuVertex, PacksMeshAttributesIntoTheGpuLayout) {
    const Engine::Vertex source{
        .position = {1.0F, -2.0F, 3.0F},
        .color = {0.25F, 0.5F, 0.75F},
        .texCoord = {0.125F, 0.875F},
        .normal = {0.0F, 0.0F, 1.0F},
        .tangent = {1.0F, 0.0F, 0.0F, -1.0F},
        .texCoord1 = {0.25F, 0.75F},
        .materialIndex = 1234U,
    };

    const Engine::GpuVertex packed = Engine::GpuVertex::pack(source);
    EXPECT_EQ(sizeof(packed), 32U);
    EXPECT_FLOAT_EQ(packed.px, source.position.x());
    EXPECT_FLOAT_EQ(packed.py, source.position.y());
    EXPECT_FLOAT_EQ(packed.pz, source.position.z());
    EXPECT_EQ(packed.materialIndex, source.materialIndex);

    const glm::vec3 normal = Engine::GpuVertex::unpackNormal(packed.tangentFrame);
    const glm::vec4 tangent = Engine::GpuVertex::unpackTangent(packed.tangentFrame);
    const glm::vec2 uv0 = glm::unpackHalf2x16(packed.texCoord);
    const glm::vec2 uv1 = glm::unpackHalf2x16(packed.texCoord1);
    const glm::vec4 color = glm::unpackUnorm4x8(packed.color);
    EXPECT_NEAR(normal.z, 1.0F, 0.01F);
    EXPECT_NEAR(tangent.x, 1.0F, 0.01F);
    EXPECT_NEAR(tangent.w, -1.0F, 0.01F);
    EXPECT_NEAR(uv0.x, source.texCoord.x(), 0.001F);
    EXPECT_NEAR(uv0.y, source.texCoord.y(), 0.001F);
    EXPECT_NEAR(uv1.x, source.texCoord1.x(), 0.001F);
    EXPECT_NEAR(uv1.y, source.texCoord1.y(), 0.001F);
    EXPECT_NEAR(color.r, source.color.x(), 0.01F);
    EXPECT_NEAR(color.g, source.color.y(), 0.01F);
    EXPECT_NEAR(color.b, source.color.z(), 0.01F);
    EXPECT_FLOAT_EQ(color.a, 1.0F);
}

TEST(GpuVertex, TangentFrameKeepsNormalsAndTangentsWithinQuantizationError) {
    // A deterministic spread of directions, including the poles and the
    // octahedral fold (negative z), with tangents in every quadrant.
    std::uint32_t state = 12345U;
    const auto next = [&state] {
        state = state * 1664525U + 1013904223U;
        return static_cast<float>(state >> 8U) / static_cast<float>(1U << 24U) * 2.0F - 1.0F;
    };
    std::vector<glm::vec3> normals{{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {0, -1, 0}, {0.6F, 0.0F, -0.8F}};
    for (int i = 0; i < 4000; ++i) {
        const glm::vec3 candidate{next(), next(), next()};
        if (glm::length(candidate) > 1.0e-3F)
            normals.push_back(glm::normalize(candidate));
    }
    float worstNormal = 1.0F;
    float worstTangent = 1.0F;
    for (const glm::vec3& normal : normals) {
        glm::vec3 helper = std::abs(normal.y) < 0.9F ? glm::vec3{0, 1, 0} : glm::vec3{1, 0, 0};
        helper = glm::normalize(helper + glm::vec3{next(), next(), next()} * 0.3F);
        const glm::vec3 direction = glm::normalize(glm::cross(normal, helper));
        const float handedness = next() < 0.0F ? -1.0F : 1.0F;
        const std::uint32_t frame = Engine::GpuVertex::packTangentFrame(normal, glm::vec4{direction, handedness});
        const glm::vec3 decodedNormal = Engine::GpuVertex::unpackNormal(frame);
        const glm::vec4 decodedTangent = Engine::GpuVertex::unpackTangent(frame);
        worstNormal = std::min(worstNormal, glm::dot(decodedNormal, normal));
        worstTangent = std::min(worstTangent, glm::dot(glm::vec3{decodedTangent}, direction));
        EXPECT_EQ(decodedTangent.w, handedness);
        EXPECT_NEAR(glm::dot(glm::vec3{decodedTangent}, decodedNormal), 0.0F, 1.0e-4F);
    }
    // 11-bit octahedral normals: below 0.1 degrees. 512 tangent angles: below
    // 0.4 degrees plus the normal error.
    EXPECT_GT(worstNormal, std::cos(glm::radians(0.1F)));
    EXPECT_GT(worstTangent, std::cos(glm::radians(0.5F)));
}

TEST(PhysicsCommandBuffer, ClearsVelocityOverridesAndTeleports) {
    Engine::PhysicsCommandBuffer commands;
    commands.linearVelocity = Engine::Vec3{1.0F, 2.0F, 3.0F};
    commands.angularVelocity = Engine::Vec3{4.0F, 5.0F, 6.0F};
    commands.teleportPosition = Engine::Vec3{7.0F, 8.0F, 9.0F};
    commands.teleportRotation = Engine::Vec3{0.0F, 90.0F, 0.0F};
    commands.clear();
    EXPECT_FALSE(commands.linearVelocity.has_value());
    EXPECT_FALSE(commands.angularVelocity.has_value());
    EXPECT_FALSE(commands.teleportPosition.has_value());
    EXPECT_FALSE(commands.teleportRotation.has_value());
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
    const Engine::PBRMaterial material;
    EXPECT_FLOAT_EQ(material.roughness, 0.55F);
    EXPECT_EQ(material.baseColorTexture, -1);
    EXPECT_EQ(material.alphaMode, Engine::AlphaMode::Opaque);
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
    EXPECT_EQ(config.antialiasing, Engine::AntialiasingLevel::TAA);
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

TEST(MeshRendererComponent, UploadedHandleDoesNotRequireDecodedSourceData) {
    auto source = std::make_shared<Engine::Mesh>();
    source->vertices.resize(3);
    source->indices = {0, 1, 2};
    source->sourcePath = "uploaded-test.mesh";

    Engine::MeshRendererComponent renderer;
    renderer.mesh = source;
    ASSERT_TRUE(renderer.hasMesh());

    const auto resource = renderer.mesh.resource();
    ASSERT_NE(resource, nullptr);
    resource->handle = Engine::MeshId{7};
    resource->vertexCount = 3;
    resource->indexCount = 3;
    renderer.mesh.releaseSourceReference();

    EXPECT_FALSE(renderer.mesh.hasSourceData());
    EXPECT_TRUE(renderer.mesh.uploaded());
    EXPECT_TRUE(renderer.hasMesh());
}

TEST(TerrainComponent, BuildsCheckerboardGridAndSculptsHeightmap) {
    Engine::TerrainComponent terrain{5, 4.0F, 4.0F, -2.0F, 2.0F};
    ASSERT_TRUE(terrain.valid());
    const Engine::Mesh flatMesh = terrain.createMesh();
    EXPECT_EQ(flatMesh.vertices.size(), 25u);
    EXPECT_EQ(flatMesh.indices.size(), 96u);
    // Vertex colour is the per-sample paint colour, so a fresh terrain is
    // unpainted. Painting must reach the rebuilt vertices unchanged.
    EXPECT_FLOAT_EQ(flatMesh.vertices[0].color.x(), 0.0F);
    EXPECT_FLOAT_EQ(flatMesh.vertices[0].color.y(), 0.0F);
    EXPECT_TRUE(terrain.paint(-2.0F, -2.0F, 1.0F, Engine::Vec3{0.25F, 0.5F, 0.75F}, 1.0F));
    const Engine::Mesh paintedMesh = terrain.createMesh();
    EXPECT_FLOAT_EQ(paintedMesh.vertices[0].color.x(), 0.25F);
    EXPECT_FLOAT_EQ(paintedMesh.vertices[0].color.y(), 0.5F);
    EXPECT_FLOAT_EQ(paintedMesh.vertices[0].color.z(), 0.75F);
    const auto& first = flatMesh.vertices[flatMesh.indices[0]];
    const auto& second = flatMesh.vertices[flatMesh.indices[1]];
    const auto& third = flatMesh.vertices[flatMesh.indices[2]];
    EXPECT_GT(Engine::cross(second.position - first.position, third.position - first.position).y(), 0.0F);

    EXPECT_TRUE(terrain.sculpt(0.0F, 0.0F, 1.5F, 0.5F, Engine::TerrainSculptMode::Raise));
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

namespace {
Engine::GPUMaterialData gpuGlassDefaults() {
    return {};
}
} // namespace

TEST(MaterialTypes, ExposeGpuFriendlyDefaultsAndLayerConfiguration) {
    EXPECT_EQ(Engine::MaxMaterialTextures, 4096u);
    EXPECT_EQ(alignof(Engine::GPUMaterialData), 16u);
    EXPECT_EQ(offsetof(Engine::GPUMaterialData, displacementParams), 96u);
    EXPECT_EQ(offsetof(Engine::GPUMaterialData, extensionTextureIndices), 112u);
    EXPECT_EQ(offsetof(Engine::GPUMaterialData, waterWaveCount), 816u);
    // The glass block follows the water block: six vec4 and two arrays of three.
    EXPECT_EQ(offsetof(Engine::GPUMaterialData, glassParams), 832u);
    EXPECT_EQ(offsetof(Engine::GPUMaterialData, glassTextureTransforms), 896u);
    EXPECT_EQ(sizeof(Engine::GPUMaterialData), 992u);
    EXPECT_EQ(gpuGlassDefaults().glassTextureIndices[0], -1);
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

TEST(LightProfiles, BuiltInShapesPeakAtFullIntensity) {
    for (int index = 1; index < Engine::LightProfileCount - 1; ++index) {
        const auto profile = static_cast<Engine::LightProfile>(index);
        const auto table = Engine::builtInLightProfile(profile);
        EXPECT_EQ(*std::ranges::max_element(table), 255) << Engine::lightProfileName(profile);
    }
    const auto downlight = Engine::builtInLightProfile(Engine::LightProfile::Downlight);
    EXPECT_EQ(downlight.front(), 255);
    EXPECT_LT(downlight[20], 16); // ~116 degrees: above the horizon
    const auto uniform = Engine::builtInLightProfile(Engine::LightProfile::None);
    EXPECT_TRUE(std::ranges::all_of(uniform, [](const std::uint8_t value) { return value == 255; }));
}

TEST(LightProfiles, ParsesIesDownlightAndAveragesHorizontalAngles) {
    // Type C, vertical 0-90 (dark above the horizon), two horizontal planes.
    const std::string_view ies = "IESNA:LM-63-2002\n"
                                 "[TEST] unit\n"
                                 "[MANUFAC] engine\n"
                                 "TILT=NONE\n"
                                 "1 1000 1 3 2 1 2 0 0 0\n"
                                 "1 1 50\n"
                                 "0 45 90\n"
                                 "0 90\n"
                                 "1000 500 0\n"
                                 "800 300 0\n";
    const auto table = Engine::parseIesProfile(ies);
    ASSERT_TRUE(table.has_value());
    EXPECT_EQ((*table)[0], 255);              // nadir: (1000 + 800) / 2, the peak
    EXPECT_NEAR((*table)[8], 110, 3);         // ~46 degrees: ~387 / 900
    EXPECT_EQ((*table)[16], 0);               // ~93 degrees: outside the data
    EXPECT_EQ((*table)[31], 0);
}

TEST(LightProfiles, RejectsMalformedIes) {
    EXPECT_FALSE(Engine::parseIesProfile("IESNA:LM-63-2002\nno tilt line\n1 2 3").has_value());
    EXPECT_FALSE(Engine::parseIesProfile("TILT=NONE\n1 1000 1 3").has_value());
    EXPECT_FALSE(Engine::parseIesProfile("TILT=NONE\n1 1000 1 2 1 1 2 0 0 0 1 1 50 0 90 0 0 0").has_value());
}

TEST(LightProfiles, PacksFourSamplesPerWordInOrder) {
    Engine::LightProfileTable table{};
    for (std::size_t index = 0; index < table.size(); ++index)
        table[index] = static_cast<std::uint8_t>(index * 8);
    const auto packed = Engine::packLightProfile(table);
    EXPECT_EQ(packed[0].x, 0x18100800U);
    EXPECT_EQ(packed[0].y, 0x38302820U);
    EXPECT_EQ(packed[1].w, 0xf8f0e8e0U);
}
