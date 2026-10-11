#include "Engine/Renderer/Lighting/VsmPagePolicy.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>

namespace Engine {
TEST(VsmPagePolicyTests, ScrollPreservesTheWorldSpacePage) {
    const glm::mat4 oldMatrix{1.0F};
    glm::mat4 newMatrix{oldMatrix};
    newMatrix[3][0] = -2.0F / 32.0F;
    newMatrix[3][1] = 4.0F / 32.0F;
    std::int32_t x{}, y{};
    ASSERT_TRUE(vsmClipPageShift(oldMatrix, newMatrix, 32, x, y));
    EXPECT_EQ(x, -1);
    EXPECT_EQ(y, 2);
    const glm::vec4 receiver{0.2F, -0.3F, 0.4F, 1.0F};
    const glm::vec2 oldPage = (glm::vec2(oldMatrix * receiver) * 0.5F + 0.5F) * 32.0F;
    const glm::vec2 newPage = (glm::vec2(newMatrix * receiver) * 0.5F + 0.5F) * 32.0F;
    EXPECT_NEAR(newPage.x - oldPage.x, x, 1e-5F);
    EXPECT_NEAR(newPage.y - oldPage.y, y, 1e-5F);
}

TEST(VsmPagePolicyTests, SubpageScrollCannotReuseDepth) {
    const glm::mat4 oldMatrix{1.0F};
    auto next = oldMatrix;
    next[3][0] = 1.0F / 32.0F;
    std::int32_t x{}, y{};
    EXPECT_FALSE(vsmClipPageShift(oldMatrix, next, 32, x, y));
}

TEST(VsmPagePolicyTests, EvenSmallDepthOrBasisChangesInvalidatePages) {
    const glm::mat4 oldMatrix{1.0F};
    std::int32_t x{}, y{};
    auto next = oldMatrix;
    next[3][2] = 1.0e-6F;
    EXPECT_FALSE(vsmClipPageShift(oldMatrix, next, 32, x, y));
    next = oldMatrix;
    next[0][0] += 1.0e-6F;
    EXPECT_FALSE(vsmClipPageShift(oldMatrix, next, 32, x, y));
}

TEST(VsmPagePolicyTests, InvalidAndOutOfRangeMatricesCannotOverflowShift) {
    const glm::mat4 oldMatrix{1.0F};
    std::int32_t x{}, y{};
    for (const float value : {std::numeric_limits<float>::infinity(),
                             std::numeric_limits<float>::quiet_NaN(), 1e30F, 2.0F}) {
        auto next = oldMatrix;
        next[3][0] = value;
        EXPECT_FALSE(vsmClipPageShift(oldMatrix, next, 32, x, y));
    }
    auto next = oldMatrix;
    next[3][3] = 0.0F;
    EXPECT_FALSE(vsmClipPageShift(oldMatrix, next, 32, x, y));
}

TEST(VsmPagePolicyTests, StationaryOversubscribedWorkingSetDoesNotChurn) {
    std::array<bool, 1024> resident{};
    std::array<std::uint32_t, 128> keys{};
    for (std::uint32_t i = 0; i < keys.size(); ++i) keys[i] = i;
    for (std::uint32_t frame = 0; frame < 120; ++frame) {
        std::reverse(keys.begin(), keys.end()); // Unspecified GPU compaction order.
        std::ranges::sort(keys, [&](auto a, auto b) {
            return vsmRequestBefore(a, b, resident[a], resident[b], !resident[a], !resident[b], 1024);
        });
        for (std::uint32_t i = 0; i < 48; ++i) {
            EXPECT_EQ(keys[i], i);
            resident[keys[i]] = true;
        }
    }
}

TEST(VsmPagePolicyTests, LeavingRequestsAllowsNewPagesIntoTheWorkingSet) {
    EXPECT_TRUE(vsmRequestBefore(30, 10, true, false, false, true, 1024));
    EXPECT_TRUE(vsmRequestBefore(10, 20, false, false, true, false, 1024));
    EXPECT_TRUE(vsmRequestBefore(20, 30, false, false, false, false, 1024));
    EXPECT_TRUE(vsmRequestBefore(20, 1050, false, true, false, false, 1024));
    EXPECT_FALSE(vsmRequestBefore(20, 20, true, true, false, false, 1024));
}

TEST(VsmPagePolicyTests, ContinuousAnimationRefreshesEveryTileWithSmallBudget) {
    std::array<std::uint64_t, 256> rendered{};
    std::array<std::uint32_t, 256> pages{};
    for (std::uint32_t i = 0; i < pages.size(); ++i) pages[i] = i;
    for (std::uint32_t frame = 1; frame <= 16; ++frame) {
        std::ranges::sort(pages, [&](auto a, auto b) {
            return vsmRefreshBefore(a, b, rendered[a], rendered[b], a < 100 ? 6 : 0, b < 100 ? 6 : 0);
        });
        for (std::uint32_t i = 0; i < 32; ++i) rendered[pages[i]] = frame;
        if (frame % 8 == 0) {
            for (auto last : rendered) { EXPECT_GT(last, frame - 8); }
        }
    }
}
}
