#include <gtest/gtest.h>

#include "Engine/Renderer/Culling/CullingTypes.h"
#include "Engine/Input/KeyCode.h"
#include "Engine/Input/MouseButton.h"
#include "Engine/UI/UIElement.h"
#include "Engine/UI/UIVertex.h"

#include <cstdint>
#include <memory>

namespace {

TEST(UIElement, UpdatesLayoutRecursivelyForChildren) {
    auto root = std::make_unique<Engine::UI::UIElement>();
    root->rectTransform.anchorMin = {0.0F, 0.0F};
    root->rectTransform.anchorMax = {1.0F, 1.0F};
    root->rectTransform.offsetMin = {10.0F, 20.0F};
    root->rectTransform.offsetMax = {-30.0F, -40.0F};

    auto child = std::make_unique<Engine::UI::UIElement>();
    child->rectTransform.anchorMin = {0.5F, 0.5F};
    child->rectTransform.anchorMax = {0.5F, 0.5F};
    child->rectTransform.offsetMin = {-10.0F, -5.0F};
    child->rectTransform.offsetMax = {30.0F, 15.0F};
    root->addChild(std::move(child));

    root->updateLayout({0.0F, 0.0F, 200.0F, 100.0F});
    ASSERT_EQ(root->children().size(), 1u);
    const auto& rootRect = root->rectTransform.calculatedRect;
    EXPECT_FLOAT_EQ(rootRect.x, 10.0F);
    EXPECT_FLOAT_EQ(rootRect.y, 20.0F);
    EXPECT_FLOAT_EQ(rootRect.width, 160.0F);
    EXPECT_FLOAT_EQ(rootRect.height, 40.0F);
    const auto& childRect = root->children().front()->rectTransform.calculatedRect;
    EXPECT_FLOAT_EQ(childRect.x, 80.0F);
    EXPECT_FLOAT_EQ(childRect.y, 35.0F);
    EXPECT_FLOAT_EQ(childRect.width, 40.0F);
    EXPECT_FLOAT_EQ(childRect.height, 20.0F);
}

TEST(UIElement, StoresVisualStateAndOwnsChildren) {
    Engine::UI::UIElement element;
    EXPECT_TRUE(element.visible);
    EXPECT_EQ(element.sortingOrder, 0);
    EXPECT_EQ(element.geometryRevision(), 0u);
    element.visible = false;
    element.sortingOrder = 42;
    element.addChild(std::make_unique<Engine::UI::UIElement>());
    EXPECT_FALSE(element.visible);
    EXPECT_EQ(element.sortingOrder, 42);
    EXPECT_EQ(element.children().size(), 1u);
}

TEST(UIVertex, StoresDataAndReportsItsOwnBinarySize) {
    Engine::UI::UIVertex vertex{
        .position = {1.0F, 2.0F},
        .uv = {0.25F, 0.75F},
        .color = Engine::Color::red(),
        .textSample = 1.0F,
    };
    EXPECT_FLOAT_EQ(vertex.position.x(), 1.0F);
    EXPECT_FLOAT_EQ(vertex.uv.y(), 0.75F);
    EXPECT_FLOAT_EQ(vertex.color.r(), 1.0F);
    EXPECT_FLOAT_EQ(vertex.textSample, 1.0F);
    EXPECT_EQ(Engine::UI::UIVertex::size(), sizeof(vertex));
}

TEST(CullingTypes, HaveExpectedGpuAlignmentAndContiguousMatrixStorage) {
    EXPECT_EQ(alignof(Engine::Culling::GPUVec4), 16u);
    EXPECT_EQ(sizeof(Engine::Culling::GPUVec4), 16u);
    EXPECT_EQ(alignof(Engine::Culling::GPUMat4), 16u);
    EXPECT_EQ(sizeof(Engine::Culling::GPUMat4), 64u);
    Engine::Culling::GPUMat4 matrix{};
    for (std::uint32_t i = 0; i < 16; ++i) matrix.data[i] = static_cast<float>(i);
    EXPECT_FLOAT_EQ(matrix.data[0], 0.0F);
    EXPECT_FLOAT_EQ(matrix.data[15], 15.0F);
}

TEST(CullingTypes, PreserveObjectAndUniformFieldValues) {
    Engine::Culling::GPUObjectData object{};
    object.localAabbMin = {-1.0F, -2.0F, -3.0F, 0.0F};
    object.localAabbMax = {1.0F, 2.0F, 3.0F, 0.0F};
    object.indexCount = 36;
    object.instanceCount = 2;
    object.castShadow = 1;
    EXPECT_FLOAT_EQ(object.localAabbMin.y, -2.0F);
    EXPECT_EQ(object.indexCount, 36u);
    EXPECT_EQ(object.instanceCount, 2u);
    EXPECT_EQ(object.castShadow, 1u);

    Engine::Culling::CullingUniformData uniform{};
    uniform.objectCount = 10;
    uniform.maxDrawCount = 8;
    uniform.enableOcclusionCulling = 1;
    uniform.viewportWidth = 1920.0F;
    uniform.viewportHeight = 1080.0F;
    EXPECT_EQ(uniform.objectCount, 10u);
    EXPECT_EQ(uniform.maxDrawCount, 8u);
    EXPECT_EQ(uniform.enableOcclusionCulling, 1u);
    EXPECT_FLOAT_EQ(uniform.viewportWidth / uniform.viewportHeight, 16.0F / 9.0F);
}

TEST(CullingTypes, ObjectAndUniformDataHaveGpuCompatibleAlignment) {
    EXPECT_EQ(alignof(Engine::Culling::GPUObjectData), 16u);
    EXPECT_EQ(alignof(Engine::Culling::CullingUniformData), 16u);
    EXPECT_EQ(sizeof(Engine::Culling::GPUObjectData) % 16u, 0u);
    EXPECT_EQ(sizeof(Engine::Culling::CullingUniformData) % 16u, 0u);
}

TEST(InputEnums, ExposeStableDistinctButtonAndKeyValues) {
    EXPECT_EQ(static_cast<std::uint8_t>(Engine::MouseButton::Left), 0u);
    EXPECT_EQ(static_cast<std::uint8_t>(Engine::MouseButton::Count), 5u);
    EXPECT_EQ(static_cast<std::uint16_t>(Engine::KeyCode::Unknown), 0u);
    EXPECT_NE(Engine::KeyCode::A, Engine::KeyCode::B);
    EXPECT_NE(Engine::KeyCode::Left, Engine::KeyCode::Right);
    EXPECT_GT(static_cast<std::uint16_t>(Engine::KeyCode::Count), 1u);
}

} // namespace
