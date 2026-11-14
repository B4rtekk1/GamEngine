#include "Engine/Renderer/Vulkan/renderer_types.h"
#include "Engine/Scene/Components/IdentityComponents.h"
#include "Engine/Scene/TransformSystem.h"

#include <gtest/gtest.h>

namespace {
TEST(TransformSystem, DirtyLeafDoesNotChangeSiblingWorldRevisions) {
    Engine::Registry registry;
    const auto root = registry.create();
    const auto leaf = registry.create();
    const auto sibling = registry.create();
    for (const auto entity : {root, leaf, sibling})
        registry.add<Engine::Transform>(entity);
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

TEST(TransformSystem, ChangeLogKeepsUpdatesResolvedByAnEarlierCaller) {
    // A camera moved by a script is resolved by the physics step's update;
    // the renderer's later update then has an unrelated (or empty) batch.
    // Reading changes since its stored serial must still report the camera.
    Engine::Registry registry;
    const auto camera = registry.create();
    const auto body = registry.create();
    registry.add<Engine::Transform>(camera);
    registry.add<Engine::Transform>(body);
    Engine::TransformSystem::updateDirty(registry);
    const std::uint64_t consumed = Engine::TransformSystem::worldTransformSerial(registry);

    registry.modify<Engine::Transform>(camera, [](auto& transform) { transform.rotation = {0, 5, 0}; });
    Engine::TransformSystem::updateDirty(registry); // physics step
    registry.modify<Engine::Transform>(body, [](auto& transform) { transform.position = {1, 0, 0}; });
    Engine::TransformSystem::updateDirty(registry); // physics interpolation
    Engine::TransformSystem::updateDirty(registry); // renderer: nothing left

    const auto latest = Engine::TransformSystem::changedWorldTransforms(registry);
    ASSERT_EQ(latest.size(), 1U);
    EXPECT_EQ(latest.front(), body);
    bool complete = false;
    const auto changes = Engine::TransformSystem::worldTransformChangesSince(registry, consumed, complete);
    EXPECT_TRUE(complete);
    ASSERT_EQ(changes.size(), 2U);
    EXPECT_EQ(changes[0].entity, camera);
    EXPECT_EQ(changes[1].entity, body);
    const std::uint64_t now = Engine::TransformSystem::worldTransformSerial(registry);
    EXPECT_TRUE(Engine::TransformSystem::worldTransformChangesSince(registry, now, complete).empty());
    EXPECT_TRUE(complete);

    // After the registry contents are replaced, an old serial is incomplete.
    Engine::TransformSystem::invalidate(registry);
    Engine::TransformSystem::updateDirty(registry);
    (void)Engine::TransformSystem::worldTransformChangesSince(registry, consumed, complete);
    EXPECT_FALSE(complete);
    Engine::TransformSystem::invalidate(registry);
}

TEST(TransformComponent, ClosedFormMatrixMatchesRotationChain) {
    // matrix() is a closed form of T * Rx * Ry * Rz * S; keep it equal to
    // the generic axis-angle product it replaced.
    const float angles[] = {-270.0F, -90.0F, -33.5F, 0.0F, 12.25F, 45.0F, 90.0F, 179.0F, 400.0F};
    for (const float x : angles) {
        for (const float y : angles) {
            for (const float z : angles) {
                Engine::Transform transform;
                transform.position = {1.5F, -2.0F, 3.25F};
                transform.rotation = {x, y, z};
                transform.scale = {2.0F, 0.5F, -1.25F};
                Engine::Mat4 expected = Engine::Mat4::translate(transform.position);
                expected = Engine::Mat4::rotate(expected, Engine::Radians{Engine::Degrees{x}}, {1.0F, 0.0F, 0.0F});
                expected = Engine::Mat4::rotate(expected, Engine::Radians{Engine::Degrees{y}}, {0.0F, 1.0F, 0.0F});
                expected = Engine::Mat4::rotate(expected, Engine::Radians{Engine::Degrees{z}}, {0.0F, 0.0F, 1.0F});
                expected = Engine::Mat4::scale(expected, transform.scale);
                const glm::mat4 actual = transform.matrix().native();
                for (int column = 0; column < 4; ++column)
                    for (int row = 0; row < 4; ++row)
                        EXPECT_NEAR(actual[column][row], expected.native()[column][row], 1.0e-5F)
                            << "rotation " << x << ", " << y << ", " << z;
            }
        }
    }
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
        if ((dirty & bit) == 0)
            continue;
        ++uploads;
        const bool historyChanged = previous.settle(current);
        dirty &= ~bit;
        if (historyChanged)
            dirty |= 0b111;
    }
    EXPECT_EQ(dirty, 0U);
    EXPECT_EQ(uploads, 4U);
}
} // namespace
