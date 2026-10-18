#include "Engine/Renderer/Passes/ShadowPass.h"

#include <algorithm>
#include <gtest/gtest.h>

namespace Engine {
namespace {
std::array<Mat4, ShadowMap::ClipLevelCount> clipmaps() {
    std::array<Mat4, ShadowMap::ClipLevelCount> result;
    result.fill(Mat4{glm::mat4{1.0F}});
    return result;
}
bool unmapped(const ShadowPass& pass) {
    return std::ranges::all_of(pass.pageTable(), [](auto page) { return page == ShadowMap::InvalidPage; });
}
} // namespace

TEST(VsmPageCacheTests, NewCacheHasNoPublishedMappings) {
    ShadowPass pass;
    EXPECT_TRUE(unmapped(pass));
    EXPECT_FALSE(pass.hasPendingPageUpdates());
}

TEST(VsmPageCacheTests, ZeroBudgetKeepsCoveragePendingWithoutPublishingStaleDepth) {
    ShadowPass pass;
    const auto matrices = clipmaps();
    pass.preparePages(matrices, Mat4{glm::mat4{1.0F}}, {}, {}, {}, true, 0, 0);
    EXPECT_TRUE(pass.grassPageMatrices(matrices).empty());
    EXPECT_TRUE(pass.hasPendingPageUpdates());
    EXPECT_TRUE(unmapped(pass));
}

TEST(VsmPageCacheTests, PartialFallbackNeedsAnotherSceneViewFrame) {
    ShadowPass pass;
    const auto matrices = clipmaps();
    pass.preparePages(matrices, Mat4{glm::mat4{1.0F}}, {}, {}, {}, true, 0, 32);
    EXPECT_EQ(pass.grassPageMatrices(matrices).size(), 32U);
    EXPECT_TRUE(pass.hasPendingPageUpdates());
    EXPECT_TRUE(unmapped(pass));
}

TEST(VsmPageCacheTests, FullFallbackFitsBudgetEvenWithoutReceivers) {
    ShadowPass pass;
    const auto matrices = clipmaps();
    pass.preparePages(matrices, Mat4{glm::mat4{1.0F}}, {}, {}, {}, true, 0, 256);
    EXPECT_EQ(pass.grassPageMatrices(matrices).size(), ShadowMap::FallbackPageCount);
    EXPECT_FALSE(pass.hasPendingPageUpdates());
    EXPECT_TRUE(unmapped(pass));
}

TEST(VsmPageCacheTests, SkippedRenderDoesNotLeakPhysicalPageOwnership) {
    ShadowPass pass;
    const auto matrices = clipmaps();
    for (std::uint32_t frame = 0; frame < 20; ++frame) {
        pass.preparePages(matrices, Mat4{glm::mat4{1.0F}}, {}, {}, {}, true, frame % 2, 32);
        EXPECT_EQ(pass.grassPageMatrices(matrices).size(), 32U);
        EXPECT_TRUE(pass.hasPendingPageUpdates());
        EXPECT_TRUE(unmapped(pass));
    }
}

TEST(VsmPageCacheTests, OutOfRangeReceiverKeysAreIgnored) {
    ShadowPass pass;
    const auto matrices = clipmaps();
    const std::array requests{ShadowMap::InvalidPage, ShadowMap::VirtualPageCount, 0U, 0U};
    pass.preparePages(matrices, Mat4{glm::mat4{1.0F}}, {}, {}, requests, true, 0, 256);
    EXPECT_EQ(pass.grassPageMatrices(matrices).size(), ShadowMap::FallbackPageCount + 1);
    EXPECT_TRUE(unmapped(pass));
}
TEST(VsmPageCacheTests, FineLevelBorrowsUnusedQuotasWhilePreservingFallback) {
    ShadowPass pass;
    const auto matrices = clipmaps();
    std::vector<std::uint32_t> requests(100);
    for (std::uint32_t i = 0; i < requests.size(); ++i)
        requests[i] = i;
    pass.preparePages(matrices, Mat4{glm::mat4{1.0F}}, {}, {}, requests, true, 0, 256);
    EXPECT_EQ(pass.grassPageMatrices(matrices).size(), ShadowMap::FallbackPageCount + 100);
    EXPECT_FALSE(pass.hasPendingPageUpdates());
}

TEST(VsmPageCacheTests, BorrowingNeverExceedsPhysicalAtlasCapacity) {
    ShadowPass pass;
    const auto matrices = clipmaps();
    std::vector<std::uint32_t> requests(ShadowMap::VirtualPageCount);
    for (std::uint32_t i = 0; i < requests.size(); ++i)
        requests[i] = i;
    pass.preparePages(matrices, Mat4{glm::mat4{1.0F}}, {}, {}, requests, true, 0, 256);
    EXPECT_EQ(pass.grassPageMatrices(matrices).size(), ShadowMap::PhysicalPageCount);
    EXPECT_FALSE(pass.hasPendingPageUpdates());
}

} // namespace Engine
