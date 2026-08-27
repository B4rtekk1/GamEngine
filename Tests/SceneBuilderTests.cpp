#include <gtest/gtest.h>

#include "Engine/Scene/SceneBuilder.h"

#include <memory>

namespace {

TEST(SceneBuilder, CreatesEntitiesWithNameAndUniqueIdentifier) {
    Engine::Registry registry;
    Engine::SceneBuilder builder{registry};
    const auto first = builder.createEntity("Player");
    const auto second = builder.createEntity();
    ASSERT_TRUE(registry.valid(first));
    EXPECT_EQ(registry.get<Engine::NameComponent>(first).value, "Player");
    EXPECT_NE(registry.get<Engine::UUIDComponent>(first).value, Engine::NullUUID);
    EXPECT_NE(registry.get<Engine::UUIDComponent>(first).value,
              registry.get<Engine::UUIDComponent>(second).value);
    EXPECT_EQ(registry.get<Engine::NameComponent>(second).value, "GameObject");
}

TEST(SceneBuilder, CreatesCameraAndLightWithRequestedComponents) {
    Engine::Registry registry;
    Engine::SceneBuilder builder{registry};
    Engine::Transform cameraTransform;
    cameraTransform.position = {1.0F, 2.0F, 3.0F};
    Engine::CameraComponent camera;
    camera.setOrthographic(20.0F, 0.5F, 100.0F);
    const auto cameraEntity = builder.createCamera(cameraTransform, camera, "Main Camera");
    EXPECT_TRUE(registry.has<Engine::Transform>(cameraEntity));
    EXPECT_TRUE(registry.has<Engine::CameraComponent>(cameraEntity));
    EXPECT_TRUE(registry.get<Engine::CameraComponent>(cameraEntity).isOrthographic());
    EXPECT_FLOAT_EQ(registry.get<Engine::Transform>(cameraEntity).position.z(), 3.0F);
    EXPECT_EQ(registry.get<Engine::NameComponent>(cameraEntity).value, "Main Camera");

    Engine::LightComponent light;
    light.type = Engine::LightType::Spot;
    light.intensity = 2.5F;
    const auto lightEntity = builder.createLight({}, light);
    EXPECT_TRUE(registry.has<Engine::LightComponent>(lightEntity));
    EXPECT_EQ(registry.get<Engine::LightComponent>(lightEntity).type, Engine::LightType::Spot);
    EXPECT_FLOAT_EQ(registry.get<Engine::LightComponent>(lightEntity).intensity, 2.5F);
}

TEST(SceneBuilder, CreatesMeshEntityWithIndependentRenderSettings) {
    Engine::Registry registry;
    Engine::SceneBuilder builder{registry};
    auto mesh = std::make_shared<Engine::Mesh>();
    mesh->vertices.resize(3);
    mesh->indices = {0, 1, 2};
    Engine::Transform transform;
    transform.scale = {2.0F, 3.0F, 4.0F};
    Engine::PBRMaterial material;
    material.metallic = 0.8F;
    const auto entity = builder.createMeshEntity(mesh, transform, material, false, 7, "Mesh");
    const auto& renderer = registry.get<Engine::MeshRenderer>(entity);
    EXPECT_TRUE(renderer.hasMesh());
    EXPECT_FALSE(renderer.castShadow);
    EXPECT_EQ(renderer.cullingBatch, 7u);
    EXPECT_FLOAT_EQ(renderer.material.metallic, 0.8F);
    EXPECT_FLOAT_EQ(registry.get<Engine::Transform>(entity).scale.y(), 3.0F);
    EXPECT_EQ(registry.get<Engine::NameComponent>(entity).value, "Mesh");
}

} // namespace
