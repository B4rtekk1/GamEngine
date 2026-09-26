#include "Engine/Scene/TransformSystem.h"
#include "Engine/Scene/Components/IdentityComponents.h"
#include "Engine/Renderer/Vulkan/renderer_types.h"

#include <gtest/gtest.h>

namespace {
TEST(TransformSystem, DirtyLeafDoesNotChangeSiblingWorldRevisions) {
    Engine::Registry registry;
    const auto root = registry.create();
    const auto leaf = registry.create();
    const auto sibling = registry.create();
    for (const auto entity : {root, leaf, sibling}) registry.add<Engine::Transform>(entity);
    registry.add<Engine::UUIDComponent>(root, Engine::UUIDComponent{1});
    registry.add<Engine::ParentComponent>(leaf, Engine::ParentComponent{1});
    registry.add<Engine::ParentComponent>(sibling, Engine::ParentComponent{1});
    Engine::TransformSystem::updateDirty(registry);
    const auto rootRevision = registry.get<Engine::Transform>(root).worldRevision();
    const auto siblingRevision = registry.get<Engine::Transform>(sibling).worldRevision();
    registry.modify<Engine::Transform>(leaf, [](auto& transform) { transform.position = {3, 0, 0}; });
    Engine::TransformSystem::updateDirty(registry);
    const auto changes = Engine::TransformSystem::changedWorldTransforms(registry);
    ASSERT_EQ(changes.size(), 1U);
    EXPECT_EQ(changes.front(), leaf);
    EXPECT_EQ(registry.get<Engine::Transform>(root).worldRevision(), rootRevision);
    EXPECT_EQ(registry.get<Engine::Transform>(sibling).worldRevision(), siblingRevision);
    // Scene queries may update repeatedly before the renderer consumes changes.
    Engine::TransformSystem::updateDirty(registry);
    EXPECT_EQ(Engine::TransformSystem::changedWorldTransforms(registry).size(), 1U);
    registry.modify<Engine::Transform>(root, [](auto& transform) { transform.position = {10, 0, 0}; });
    Engine::TransformSystem::updateDirty(registry);
    EXPECT_EQ(Engine::TransformSystem::changedWorldTransforms(registry).size(), 3U);
    EXPECT_FLOAT_EQ(registry.get<Engine::Transform>(leaf).worldPosition().x(), 13.0F);
    EXPECT_FLOAT_EQ(registry.get<Engine::Transform>(sibling).worldPosition().x(), 10.0F);
    Engine::TransformSystem::invalidate(registry);
}

TEST(RendererTransformHistory, SettlesOnceAndIgnoresMaterialIndex) {
    Engine::RendererInstanceData current;
    Engine::RendererPreviousTransformData previous;
    previous.settle(current);
    EXPECT_FALSE(previous.settle(current));
    current.positionMaterial.x = 4.0F;
    EXPECT_TRUE(previous.settle(current));
    EXPECT_FALSE(previous.settle(current));
    current.positionMaterial.w = 7.0F;
    EXPECT_FALSE(previous.settle(current));
    current.rotation = {0, 1, 0, 0};
    EXPECT_TRUE(previous.settle(current));
    EXPECT_FALSE(previous.settle(current));
    current.scaleBase.x = 2.0F;
    EXPECT_TRUE(previous.settle(current));
    EXPECT_FALSE(previous.settle(current));
}

TEST(RendererTransformHistory, FrameDirtyBitsEventuallyDrainAfterMotion) {
    Engine::RendererInstanceData current;
    Engine::RendererPreviousTransformData previous;
    previous.settle(current);
    current.positionMaterial.x = 5.0F;
    unsigned dirty = 0b111;
    unsigned uploads = 0;
    for (unsigned frame = 0; frame < 12; ++frame) {
        const unsigned bit = 1U << (frame % 3);
        if ((dirty & bit) == 0) continue;
        ++uploads;
        const bool historyChanged = previous.settle(current);
        dirty &= ~bit;
        if (historyChanged) dirty |= 0b111;
    }
    EXPECT_EQ(dirty, 0U);
    EXPECT_EQ(uploads, 4U);
}
}
