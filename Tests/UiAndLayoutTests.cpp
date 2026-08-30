#include <gtest/gtest.h>

#include "Engine/Renderer/Culling/CullingTypes.h"
#include "Engine/Input/KeyCode.h"
#include "Engine/Input/MouseButton.h"
#include "Engine/UI/UIElement.h"
#include "Engine/UI/UIVertex.h"
#include "Engine/UI/Canvas.h"
#include "Engine/UI/ButtonElement.h"
#include "Engine/UI/PanelElement.h"
#include "Engine/UI/TextElement.h"
#include "Engine/UI/Vulkan/UIFontAtlas.h"

#include <cstdint>
#include <memory>
#include <stdexcept>

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

TEST(Canvas, LaysOutElementsAndTracksStructuralChanges) {
    Engine::UI::Canvas canvas{200, 100};
    EXPECT_TRUE(canvas.empty());
    EXPECT_EQ(canvas.revision(), 1u);

    auto panel = std::make_unique<Engine::UI::PanelElement>();
    panel->rectTransform.offsetMin = {10.0F, 20.0F};
    panel->rectTransform.offsetMax = {60.0F, 50.0F};
    auto& added = canvas.addElement(std::move(panel));
    EXPECT_EQ(canvas.size(), 1u);
    EXPECT_FLOAT_EQ(added.rectTransform.calculatedRect.x, 10.0F);
    EXPECT_FLOAT_EQ(added.rectTransform.calculatedRect.height, 30.0F);
    const auto afterAdd = canvas.revision();

    canvas.resize(300, 150);
    EXPECT_FLOAT_EQ(added.rectTransform.calculatedRect.width, 50.0F);
    EXPECT_GT(canvas.revision(), afterAdd);
    const auto afterResize = canvas.revision();
    canvas.resize(300, 150);
    EXPECT_EQ(canvas.revision(), afterResize);

    auto removed = canvas.removeElement(&added);
    ASSERT_NE(removed, nullptr);
    EXPECT_TRUE(canvas.empty());
    const auto absent = canvas.removeElement(removed.get());
    EXPECT_EQ(absent, nullptr);
    const auto afterRemove = canvas.revision();
    canvas.clear();
    EXPECT_EQ(canvas.revision(), afterRemove);
    EXPECT_THROW(static_cast<void>(canvas.addElement(nullptr)), std::invalid_argument);
}

TEST(UIBatch, AppendsQuadsWithContiguousVerticesAndIndices) {
    Engine::UI::UIBatch batch;
    const auto color = Engine::Color::red();
    batch.addQuad({10.0F, 20.0F, 30.0F, 40.0F}, color);
    batch.addQuad({0.0F, 0.0F, 5.0F, 6.0F}, Engine::Color::blue());
    ASSERT_EQ(batch.vertices.size(), 8u);
    ASSERT_EQ(batch.indices.size(), 12u);
    EXPECT_FLOAT_EQ(batch.vertices[0].position.x(), 10.0F);
    EXPECT_FLOAT_EQ(batch.vertices[2].position.y(), 60.0F);
    EXPECT_FLOAT_EQ(batch.vertices[3].uv.x(), 0.0F);
    EXPECT_FLOAT_EQ(batch.vertices[3].uv.y(), 1.0F);
    EXPECT_EQ(batch.indices[6], 4u);
    EXPECT_EQ(batch.indices[11], 7u);
    EXPECT_FLOAT_EQ(batch.vertices[0].textSample, 0.0F);

    batch.addQuad({0.0F, 0.0F, 0.0F, 1.0F}, color);
    batch.addQuad({0.0F, 0.0F, 1.0F, -1.0F}, color);
    EXPECT_EQ(batch.vertices.size(), 8u);
    batch.clear();
    EXPECT_TRUE(batch.empty());
    EXPECT_TRUE(batch.vertices.empty());
}

TEST(PanelAndButtonElements, BuildGeometryAndReflectVisualState) {
    Engine::UI::PanelElement panel{Engine::Color::green()};
    panel.rectTransform.calculatedRect = {1.0F, 2.0F, 3.0F, 4.0F};
    Engine::UI::UIBatch batch;
    panel.buildGeometry(batch);
    EXPECT_EQ(batch.indices.size(), 6u);
    const auto panelRevision = panel.geometryRevision();
    panel.color = Engine::Color::blue();
    EXPECT_NE(panel.geometryRevision(), panelRevision);

    Engine::UI::ButtonElement button;
    int clicks = 0;
    button.onClick = [&clicks] { ++clicks; };
    button.click();
    EXPECT_EQ(clicks, 1);
    button.rectTransform.calculatedRect = {0.0F, 0.0F, 2.0F, 2.0F};
    button.buildGeometry(batch);
    EXPECT_EQ(batch.indices.size(), 12u);
    const auto buttonRevision = button.geometryRevision();
    button.color = Engine::Color::red();
    EXPECT_NE(button.geometryRevision(), buttonRevision);
}

TEST(UIFontAtlas, StartsEmptyAndRejectsInvalidBuildRequests) {
    Engine::UI::UIFontAtlas atlas;
    EXPECT_EQ(atlas.width(), 0u);
    EXPECT_EQ(atlas.height(), 0u);
    EXPECT_EQ(atlas.pixelSize(), 0u);
    EXPECT_TRUE(atlas.pixels().empty());
    EXPECT_EQ(atlas.glyph('A'), nullptr);

    EXPECT_EQ(atlas.build({}, 16), "Invalid font atlas parameters");
    EXPECT_EQ(atlas.build("font.ttf", 0), "Invalid font atlas parameters");
    EXPECT_EQ(atlas.build("font.ttf", 16, 127, 126), "Invalid font atlas parameters");
    EXPECT_FALSE(atlas.build("missing-font.ttf", 16).empty());
    EXPECT_EQ(atlas.pixelSize(), 0u);
    EXPECT_TRUE(atlas.pixels().empty());
}

TEST(TextElement, SkipsGeometryForAnEmptyAtlasAndTracksTextChanges) {
    Engine::UI::UIFontAtlas atlas;
    Engine::TextComponent text;
    text.text = "Hello";
    Engine::UI::TextElement element{text, atlas};
    element.rectTransform.calculatedRect = {10.0F, 20.0F, 100.0F, 30.0F};
    Engine::UI::UIBatch batch;
    element.buildGeometry(batch);
    EXPECT_TRUE(batch.empty());
    const auto revision = element.geometryRevision();
    element.text.text = "World";
    EXPECT_NE(element.geometryRevision(), revision);
    element.text.visible = false;
    element.buildGeometry(batch);
    EXPECT_TRUE(batch.empty());
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
