/** @file time.cpp Time implementation. */

#include "Engine/Core/Time.h"

#include <algorithm>

namespace Engine {

double Time::s_deltaTime = 0.0;
double Time::s_unscaledDeltaTime = 0.0;
double Time::s_elapsedTime = 0.0;

double Time::s_timeScale = 1.0;
double Time::s_fixedDeltaTime = 1.0 / 60.0;

std::chrono::steady_clock::time_point Time::s_lastFrame;

void Time::init() {
    s_lastFrame = std::chrono::steady_clock::now();
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
}

void Time::setFixedDeltaTime(double deltaTime) {
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

void Time::setTimeScale(double scale) {
    s_timeScale = scale;
}

double Time::timeScale() {
    return s_timeScale;
}

} // namespace Engine
