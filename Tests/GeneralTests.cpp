#include <gtest/gtest.h>

#include "Engine/Assets/AssetHandle.h"
#include "Engine/Core/Camera.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/Renderer/Geometry/Cube.h"
#include "Engine/Renderer/Geometry/Mesh.h"
#include "Engine/Renderer/Geometry/Plane.h"
#include "Engine/Renderer/Geometry/Ramp.h"
#include "Engine/Renderer/Geometry/Sphere.h"
#include "Engine/Renderer/ViewportCamera.h"
#include "Engine/UI/RectTransform.h"

#include <memory>
#include <stdexcept>

namespace {

void ExpectVec3Near(const Engine::Vec3& value, float x, float y, float z) {
    EXPECT_NEAR(value.x(), x, 1.0e-5F);
    EXPECT_NEAR(value.y(), y, 1.0e-5F);
    EXPECT_NEAR(value.z(), z, 1.0e-5F);
}

void ExpectMat4Near(const Engine::Mat4& value, const Engine::Mat4& expected) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            EXPECT_NEAR(value.native()[column][row], expected.native()[column][row], 1.0e-5F);
        }
    }
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

TEST(Camera, AppliesRollToTheUpAndRightDirections) {
    Engine::Camera camera{Engine::Degrees{60.0F}, 1.0F, 0.1F, 100.0F};
    camera.setRotation(Engine::Degrees{-90.0F}, Engine::Degrees{0.0F}, Engine::Degrees{90.0F});

    ExpectVec3Near(camera.forward(), 0.0F, 0.0F, -1.0F);
    ExpectVec3Near(camera.right(), 0.0F, 1.0F, 0.0F);
    ExpectVec3Near(camera.up(), -1.0F, 0.0F, 0.0F);
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

TEST(PrimitiveMeshes, CubeAndPlaneHaveExpectedTopologyAndBounds) {
    const auto cube = Engine::Cube::createMesh();
    EXPECT_EQ(cube.vertices.size(), 24u);
    EXPECT_EQ(cube.indices.size(), 36u);
    for (const auto index : cube.indices) {
        EXPECT_LT(index, cube.vertices.size());
    }
    for (const auto& vertex : cube.vertices) {
        EXPECT_NEAR(vertex.position.length(), 0.8660254F, 1.0e-5F);
        EXPECT_NEAR(vertex.normal.length(), 1.0F, 1.0e-5F);
    }

    const auto plane = Engine::Plane::createMesh();
    ASSERT_EQ(plane.vertices.size(), 4u);
    EXPECT_EQ(plane.indices.size(), 6u);
    EXPECT_FLOAT_EQ(plane.vertices[0].position.y(), 0.0F);
    EXPECT_FLOAT_EQ(plane.vertices[0].position.x(), -0.5F);
    EXPECT_FLOAT_EQ(plane.vertices[2].position.z(), 0.5F);
    for (const auto& vertex : plane.vertices) {
        ExpectVec3Near(vertex.normal, 0.0F, 1.0F, 0.0F);
    }
}

TEST(PrimitiveMeshes, SphereRespectsRequestedResolutionAndUnitNormals) {
    constexpr unsigned int rings = 3;
    constexpr unsigned int segments = 4;
    const auto sphere = Engine::Sphere::createMesh(rings, segments);
    EXPECT_EQ(sphere.vertices.size(), (rings + 1U) * (segments + 1U));
    EXPECT_EQ(sphere.indices.size(), rings * segments * 6U);
    for (const auto index : sphere.indices) {
        EXPECT_LT(index, sphere.vertices.size());
    }
    for (const auto& vertex : sphere.vertices) {
        EXPECT_NEAR(vertex.position.length(), 0.5F, 1.0e-5F);
        EXPECT_NEAR(vertex.normal.length(), 1.0F, 1.0e-5F);
    }
}

TEST(PrimitiveMeshes, RampExposesExpectedDimensionsAndValidTriangles) {
    const auto extent = Engine::Ramp::halfExtents();
    ExpectVec3Near(extent, 3.0F, 2.0F, 2.0F);
    const auto ramp = Engine::Ramp::createMesh();
    EXPECT_EQ(ramp.vertices.size(), 18u);
    EXPECT_EQ(ramp.indices.size(), 30u);
    for (const auto index : ramp.indices) {
        EXPECT_LT(index, ramp.vertices.size());
    }
    EXPECT_FLOAT_EQ(ramp.vertices[0].position.y(), -extent.y());
    EXPECT_FLOAT_EQ(ramp.vertices[6].position.y(), extent.y());
    EXPECT_FLOAT_EQ(ramp.vertices[6].position.z(), extent.z());
}

TEST(ViewportCamera, BuildsGameCameraFromComponentAndTransform) {
    Engine::CameraComponent component;
    component.setPerspective(75.0F, 0.25F, 500.0F);
    Engine::Transform transform;
    transform.position = {1.0F, 2.0F, 3.0F};
    transform.rotation = {15.0F, 90.0F, 0.0F};

    const auto viewport = Engine::ViewportCamera::game(component, transform, 16.0F / 9.0F);
    EXPECT_EQ(viewport.type, Engine::ViewportCameraType::Game);
    ExpectVec3Near(viewport.camera.position(), 1.0F, 2.0F, 3.0F);
    auto expectedProjection = Engine::Mat4::perspective(
        Engine::Radians{Engine::Degrees{75.0F}}, 16.0F / 9.0F, 0.25F, 500.0F);
    expectedProjection.native()[1][1] *= -1.0F;
    ExpectMat4Near(viewport.camera.projectionMatrix(), expectedProjection);
    EXPECT_GT(viewport.camera.forward().z(), 0.9F);
    EXPECT_GT(viewport.camera.forward().y(), 0.2F);
}

TEST(ViewportCamera, BuildsStableSceneCameraDefaults) {
    const auto viewport = Engine::ViewportCamera::scene(4.0F / 3.0F);
    EXPECT_EQ(viewport.type, Engine::ViewportCameraType::Scene);
    ExpectVec3Near(viewport.camera.position(), 8.0F, 6.0F, 8.0F);
    auto expectedProjection = Engine::Mat4::perspective(
        Engine::Radians{Engine::Degrees{60.0F}}, 4.0F / 3.0F, 0.1F, 1000.0F);
    expectedProjection.native()[1][1] *= -1.0F;
    ExpectMat4Near(viewport.camera.projectionMatrix(), expectedProjection);
    EXPECT_NEAR(viewport.camera.forward().length(), 1.0F, 1.0e-5F);
}

} // namespace
