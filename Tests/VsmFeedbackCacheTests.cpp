#include "Engine/Renderer/Lighting/VsmFeedbackCache.h"
#include <gtest/gtest.h>

namespace Engine {
namespace {
    VsmFeedbackKey coverage() {
        VsmFeedbackKey key;
        key.clipMatrices.fill(glm::mat4{1.0F});
        key.width = 1920;
        key.height = 1080;
        key.quality = 2;
        return key;
    }
}

TEST(VsmFeedbackCacheTests, UnchangedCoverageReusesCompletedRequests) {
    VsmFeedbackCache cache;
    const auto key = coverage();
    EXPECT_TRUE(cache.needsUpdate(key));
    cache.commit(key);
    for (int frame = 0; frame < 120; ++frame) EXPECT_FALSE(cache.needsUpdate(key));
}

TEST(VsmFeedbackCacheTests, CameraAndClipmapChangesRequireNewFeedback) {
    VsmFeedbackCache cache;
    auto key = coverage();
    cache.commit(key);
    key.viewProjection[3][0] = 1.0F;
    EXPECT_TRUE(cache.needsUpdate(key));
    cache.commit(key);
    EXPECT_FALSE(cache.needsUpdate(key));
    key.clipMatrices[3][3][1] = 1.0F;
    EXPECT_TRUE(cache.needsUpdate(key));
}

TEST(VsmFeedbackCacheTests, ReceiverAndRenderChangesInvalidateCoverage) {
    const auto original = coverage();
    VsmFeedbackCache cache;
    cache.commit(original);
    auto changed = original;
    ++changed.sceneRevision;
    EXPECT_TRUE(cache.needsUpdate(changed));
    changed = original;
    ++changed.settingsRevision;
    EXPECT_TRUE(cache.needsUpdate(changed));
    changed = original;
    ++changed.width;
    EXPECT_TRUE(cache.needsUpdate(changed));
    changed = original;
    ++changed.height;
    EXPECT_TRUE(cache.needsUpdate(changed));
    changed = original;
    ++changed.quality;
    EXPECT_TRUE(cache.needsUpdate(changed));
    changed = original;
    ++changed.transformRevision;
    EXPECT_TRUE(cache.needsUpdate(changed));
    changed = original;
    ++changed.deformationRevision;
    EXPECT_TRUE(cache.needsUpdate(changed));
    changed = original;
    ++changed.windRevision;
    EXPECT_TRUE(cache.needsUpdate(changed));
    changed = original;
    ++changed.animationFrame;
    EXPECT_TRUE(cache.needsUpdate(changed));
}

TEST(VsmFeedbackCacheTests, FrameSlotsKeepIndependentCoverage) {
    VsmFeedbackCache first, second;
    const auto key = coverage();
    first.commit(key);
    EXPECT_FALSE(first.needsUpdate(key));
    EXPECT_TRUE(second.needsUpdate(key));
    second.commit(key);
    EXPECT_FALSE(second.needsUpdate(key));
}
}
