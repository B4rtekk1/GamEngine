#include "Engine/Renderer/GPUSceneDatabase.h"

#include <gtest/gtest.h>

namespace Engine {
    TEST(GPUSceneDatabaseTests, RetainsSlotForChangedInstance) {
        GPUSceneDatabase database;
        GPUSceneDatabase::GPUInstance instance{};
        instance.objectId = 17;
        const auto id = database.upsertInstance(17, instance);

        instance.flags = 3;
        EXPECT_EQ(database.upsertInstance(17, instance), id);
        ASSERT_EQ(database.instances().size(), 1U);
        EXPECT_EQ(database.instances()[id].flags, 3U);
        ASSERT_EQ(database.dirty().instances.size(), 1U);
        EXPECT_EQ(database.dirty().instances.front(), id);
    }

    TEST(GPUSceneDatabaseTests, ReusesRemovedInstanceSlotOnlyAfterGpuRetiresIt) {
        GPUSceneDatabase database;
        const auto first = database.upsertInstance(1, {});
        database.removeInstance(1, 7);
        const auto beforeRetirement = database.upsertInstance(2, {});

        EXPECT_NE(beforeRetirement, first);
        database.removeInstance(2, 7);
        database.reclaimDeferredInstances(6);
        EXPECT_NE(database.upsertInstance(3, {}), first);

        database.reclaimDeferredInstances(7);
        const auto reused = database.upsertInstance(4, {});
        EXPECT_EQ(reused, beforeRetirement);
        EXPECT_TRUE(database.instances()[reused].alive);
    }

    TEST(GPUSceneDatabaseTests, MeshAndMaterialAreDeduplicatedBySourceKey) {
        GPUSceneDatabase database;
        const auto mesh = database.upsertMesh(42, {.indexCount = 12});
        EXPECT_EQ(database.upsertMesh(42, {.indexCount = 24}), mesh);
        const auto material = database.upsertMaterial(7, {.pipelineClass = 2});
        EXPECT_EQ(database.upsertMaterial(7, {.pipelineClass = 3}), material);

        EXPECT_EQ(database.meshes()[mesh].indexCount, 24U);
        EXPECT_EQ(database.materials()[material].pipelineClass, 3U);
    }

    TEST(GPUSceneDatabaseTests, DirtyGenerationAllowsIdsToBeMarkedAfterClear) {
        GPUSceneDatabase database;
        const auto instance = database.upsertInstance(1, {});
        const auto mesh = database.upsertMesh(2, {});
        const auto material = database.upsertMaterial(3, {});
        database.clearDirty();

        (void) database.upsertInstance(1, {});
        (void) database.upsertMesh(2, {});
        (void) database.upsertMaterial(3, {});

        EXPECT_EQ(database.dirty().instances, std::vector{instance});
        EXPECT_EQ(database.dirty().meshes, std::vector{mesh});
        EXPECT_EQ(database.dirty().materials, std::vector{material});
    }
} // namespace Engine
