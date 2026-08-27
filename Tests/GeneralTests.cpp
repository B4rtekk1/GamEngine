#include <gtest/gtest.h>

#include "Engine/Assets/AssetHandle.h"
#include "Engine/Core/Camera.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/Renderer/Geometry/Mesh.h"
#include "Engine/UI/RectTransform.h"

#include <memory>
#include <stdexcept>

namespace {

void ExpectVec3Near(const Engine::Vec3& value, float x, float y, float z) {
    EXPECT_NEAR(value.x(), x, 1.0e-5F);
    EXPECT_NEAR(value.y(), y, 1.0e-5F);
    EXPECT_NEAR(value.z(), z, 1.0e-5F);
}

TEST(CameraComponent, DefaultsAreValidPerspectiveSettings) {
    const Engine::CameraComponent camera;
    EXPECT_TRUE(camera.isPerspective());
    EXPECT_FALSE(camera.isOrthographic());
    EXPECT_TRUE(camera.isValid());
    EXPECT_TRUE(camera.primary);
}

TEST(CameraComponent, PerspectiveConfigurationClampsInvalidParameters) {
    Engine::CameraComponent camera;
    camera.setPerspective(-10.0F, -1.0F, -5.0F);
    EXPECT_TRUE(camera.isPerspective());
    EXPECT_FLOAT_EQ(camera.fieldOfView, 1.0F);
    EXPECT_FLOAT_EQ(camera.nearClip, 0.0001F);
    EXPECT_GT(camera.farClip, camera.nearClip);
    EXPECT_TRUE(camera.isValid());
    camera.setPerspective(200.0F, 2.0F, 1.0F);
    EXPECT_FLOAT_EQ(camera.fieldOfView, 179.0F);
    EXPECT_GT(camera.farClip, camera.nearClip);
}

TEST(CameraComponent, OrthographicAndAspectRatioSettersRespectInvariants) {
    Engine::CameraComponent camera;
    camera.setOrthographic(0.0F, 5.0F, 1.0F);
    EXPECT_TRUE(camera.isOrthographic());
    EXPECT_FLOAT_EQ(camera.orthographicSize, 0.0001F);
    EXPECT_FLOAT_EQ(camera.nearClip, 5.0F);
    EXPECT_GT(camera.farClip, 5.0F);
    camera.setAspectRatio(1920.0F, 1080.0F);
    EXPECT_NEAR(camera.aspectRatio, 16.0F / 9.0F, 1.0e-6F);
    camera.setAspectRatio(-3.0F);
    EXPECT_NEAR(camera.aspectRatio, 16.0F / 9.0F, 1.0e-6F);
    camera.setAspectRatio(0.0F, 100.0F);
    EXPECT_NEAR(camera.aspectRatio, 16.0F / 9.0F, 1.0e-6F);
}

TEST(Camera, RejectsInvalidConstructionAndAspectRatio) {
    EXPECT_THROW(Engine::Camera(Engine::Degrees{0.0F}, 1.0F, 0.1F, 10.0F), std::invalid_argument);
    EXPECT_THROW(Engine::Camera(Engine::Degrees{60.0F}, 0.0F, 0.1F, 10.0F), std::invalid_argument);
    EXPECT_THROW(Engine::Camera(Engine::Degrees{60.0F}, 1.0F, 1.0F, 1.0F), std::invalid_argument);
    Engine::Camera camera{Engine::Degrees{60.0F}, 1.0F, 0.1F, 10.0F};
    EXPECT_THROW(camera.setAspectRatio(-1.0F), std::invalid_argument);
}

TEST(Camera, MovesAndProducesOrthonormalDirectionsWithClampedPitch) {
    Engine::Camera camera{Engine::Degrees{60.0F}, 1.0F, 0.1F, 100.0F};
    ExpectVec3Near(camera.forward(), 0.0F, 0.0F, -1.0F);
    camera.move({1.0F, -2.0F, 3.0F});
    ExpectVec3Near(camera.position(), 1.0F, -2.0F, 6.0F);
    camera.setRotation(Engine::Degrees{0.0F}, Engine::Degrees{100.0F});
    EXPECT_NEAR(camera.forward().y(), std::sin(89.0F * Engine::kRadiansPerDegree), 1.0e-5F);
    EXPECT_NEAR(camera.forward().length(), 1.0F, 1.0e-5F);
    EXPECT_NEAR(camera.right().length(), 1.0F, 1.0e-5F);
    EXPECT_NEAR(camera.up().length(), 1.0F, 1.0e-5F);
    EXPECT_NEAR(Engine::dot(camera.forward(), camera.right()), 0.0F, 1.0e-5F);
}

TEST(RectTransform, CalculatesFixedAndStretchedRectangles) {
    const Engine::UI::Rect parent{10.0F, 20.0F, 200.0F, 100.0F};
    Engine::UI::RectTransform fixed;
    fixed.offsetMin = {5.0F, 6.0F};
    fixed.offsetMax = {25.0F, 36.0F};
    fixed.calculate(parent);
    EXPECT_FLOAT_EQ(fixed.calculatedRect.x, 15.0F);
    EXPECT_FLOAT_EQ(fixed.calculatedRect.y, 26.0F);
    EXPECT_FLOAT_EQ(fixed.calculatedRect.width, 20.0F);
    EXPECT_FLOAT_EQ(fixed.calculatedRect.height, 30.0F);

    Engine::UI::RectTransform stretched;
    stretched.anchorMin = {0.25F, 0.5F};
    stretched.anchorMax = {0.75F, 1.0F};
    stretched.offsetMin = {2.0F, 3.0F};
    stretched.offsetMax = {-4.0F, -5.0F};
    stretched.calculate(parent);
    EXPECT_FLOAT_EQ(stretched.calculatedRect.x, 62.0F);
    EXPECT_FLOAT_EQ(stretched.calculatedRect.y, 73.0F);
    EXPECT_FLOAT_EQ(stretched.calculatedRect.width, 94.0F);
    EXPECT_FLOAT_EQ(stretched.calculatedRect.height, 42.0F);
}

TEST(AssetHandle, RepresentsEmptyAndSharedImmutableAssets) {
    Engine::Assets::AssetHandle<int> empty;
    EXPECT_FALSE(empty);
    EXPECT_EQ(empty.get(), nullptr);
    EXPECT_EQ(empty.id(), 0u);
    const auto value = std::make_shared<const int>(42);
    Engine::Assets::AssetHandle<int> handle{17, value};
    ASSERT_TRUE(handle);
    EXPECT_EQ(handle.id(), 17u);
    EXPECT_EQ(*handle, 42);
    EXPECT_EQ(*handle.get(), 42);
    EXPECT_EQ(handle.shared(), value);
    handle.reset();
    EXPECT_FALSE(handle);
    EXPECT_EQ(handle.id(), 0u);
    EXPECT_TRUE((Engine::Assets::is_asset_handle<Engine::Assets::AssetHandle<int>>::value));
    EXPECT_FALSE((Engine::Assets::is_asset_handle<int>::value));
}

TEST(AssetTypes, ConvertsEveryKnownTypeToReadableName) {
    EXPECT_EQ(Engine::Assets::to_string(Engine::Assets::AssetType::Binary), "Binary");
    EXPECT_EQ(Engine::Assets::to_string(Engine::Assets::AssetType::Texture2D), "Texture2D");
    EXPECT_EQ(Engine::Assets::to_string(Engine::Assets::AssetType::Font), "Font");
    EXPECT_EQ(Engine::Assets::to_string(Engine::Assets::AssetType::Unknown), "Unknown");
}

TEST(Mesh, ReportsEmptyStateAndCounts) {
    Engine::Mesh mesh;
    EXPECT_TRUE(mesh.empty());
    mesh.vertices.resize(3);
    EXPECT_TRUE(mesh.empty());
    mesh.indices = {0, 1, 2};
    EXPECT_FALSE(mesh.empty());
    EXPECT_EQ(mesh.vertexCount(), 3u);
    EXPECT_EQ(mesh.indexCount(), 3u);
}

} // namespace
