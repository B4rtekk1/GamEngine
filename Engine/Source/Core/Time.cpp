/** @file time.cpp Time implementation. */

#include "Engine/Core/Time.h"

#include <algorithm>
#include <stdexcept>

namespace Engine {

double Time::s_deltaTime = 0.0;
double Time::s_unscaledDeltaTime = 0.0;
double Time::s_elapsedTime = 0.0;

double Time::s_timeScale = 1.0;
double Time::s_fixedDeltaTime = 1.0 / 60.0;
double Time::s_fixedAccumulator = 0.0;

std::chrono::steady_clock::time_point Time::s_lastFrame;

void Time::init() {
    s_lastFrame = std::chrono::steady_clock::now();
    s_deltaTime = 0.0;
    s_unscaledDeltaTime = 0.0;
    s_elapsedTime = 0.0;
    s_fixedAccumulator = 0.0;
}

void Time::update() {
    const auto now = std::chrono::steady_clock::now();

    const std::chrono::duration<double> elapsed = now - s_lastFrame;
    s_lastFrame = now;

    s_unscaledDeltaTime = elapsed.count();

    constexpr double maxDelta = 0.1;
    s_unscaledDeltaTime = std::min(s_unscaledDeltaTime, maxDelta);
    s_deltaTime = s_unscaledDeltaTime * s_timeScale;
    s_elapsedTime += s_deltaTime;
    s_fixedAccumulator += s_deltaTime;
}

void Time::setFixedDeltaTime(double deltaTime) {
    if (deltaTime <= 0.0) {
        throw std::invalid_argument("Time::setFixedDeltaTime requires a positive delta time");
    }

    s_fixedDeltaTime = deltaTime;
}

double Time::deltaTime() {
    return s_deltaTime;
}

double Time::unscaledDeltaTime() {
    return s_unscaledDeltaTime;
}

double Time::elapsedTime() {
    return s_elapsedTime;
}

double Time::fixedDeltaTime() {
    return s_fixedDeltaTime;
}

bool Time::consumeFixedStep() {
    if (s_fixedAccumulator < s_fixedDeltaTime) {
        return false;
    }

    s_fixedAccumulator -= s_fixedDeltaTime;
    return true;
}

void Time::setTimeScale(double scale) {
    if (!(scale >= 0.0)) {
        throw std::invalid_argument("Time::setTimeScale requires a non-negative scale");
    }

    s_timeScale = scale;
}

double Time::timeScale() {
    return s_timeScale;
}

} // namespace Engine
