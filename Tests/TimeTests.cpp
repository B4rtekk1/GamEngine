#include <gtest/gtest.h>

#include "Engine/Core/Time.h"

#include <stdexcept>

namespace {

TEST(Time, RejectsInvalidScaleAndFixedStep) {
    EXPECT_THROW(Engine::Time::setTimeScale(-0.1), std::invalid_argument);
    EXPECT_THROW(Engine::Time::setFixedDeltaTime(0.0), std::invalid_argument);
    EXPECT_THROW(Engine::Time::setFixedDeltaTime(-1.0), std::invalid_argument);
}

TEST(Time, StoresConfigurableScaleAndFixedStep) {
    Engine::Time::setTimeScale(0.25);
    Engine::Time::setFixedDeltaTime(0.02);
    EXPECT_DOUBLE_EQ(Engine::Time::timeScale(), 0.25);
    EXPECT_DOUBLE_EQ(Engine::Time::fixedDeltaTime(), 0.02);
    Engine::Time::setTimeScale(1.0);
    Engine::Time::setFixedDeltaTime(1.0 / 60.0);
}

TEST(Time, InitResetsFrameValuesAndZeroScalePreventsAccumulation) {
    Engine::Time::setTimeScale(0.0);
    Engine::Time::init();
    EXPECT_DOUBLE_EQ(Engine::Time::deltaTime(), 0.0);
    EXPECT_DOUBLE_EQ(Engine::Time::unscaledDeltaTime(), 0.0);
    EXPECT_DOUBLE_EQ(Engine::Time::elapsedTime(), 0.0);
    EXPECT_FALSE(Engine::Time::consumeFixedStep());
    Engine::Time::update();
    EXPECT_DOUBLE_EQ(Engine::Time::deltaTime(), 0.0);
    EXPECT_DOUBLE_EQ(Engine::Time::elapsedTime(), 0.0);
    EXPECT_FALSE(Engine::Time::consumeFixedStep());
    Engine::Time::setTimeScale(1.0);
}

} // namespace
