#include <gtest/gtest.h>

#ifndef GAMEENGINE_LEGACY_TEST_FUNCTION
#error "GAMEENGINE_LEGACY_TEST_FUNCTION must name the adapted test entry point"
#endif

int GAMEENGINE_LEGACY_TEST_FUNCTION();

TEST(LegacyTest, ReturnsSuccess) {
    EXPECT_EQ(GAMEENGINE_LEGACY_TEST_FUNCTION(), 0);
}
