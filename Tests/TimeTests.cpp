#include <Engine/Core/Time.h>

#include <cmath>
#include <stdexcept>

int main() {
    using namespace Engine;

    Time::init();
    if (Time::deltaTime() != 0.0 || Time::unscaledDeltaTime() != 0.0 ||
        Time::elapsedTime() != 0.0 || Time::timeScale() != 1.0 ||
        Time::consumeFixedStep()) return 1;

    Time::setFixedDeltaTime(0.01);
    if (Time::fixedDeltaTime() != 0.01) return 2;
    try {
        Time::setFixedDeltaTime(0.0);
        return 3;
    } catch (const std::invalid_argument&) {
    }
    try {
        Time::setTimeScale(-1.0);
        return 4;
    } catch (const std::invalid_argument&) {
    }
    try {
        Time::setTimeScale(std::nan("") );
        return 5;
    } catch (const std::invalid_argument&) {
    }

    Time::setTimeScale(2.0);
    Time::update();
    const double unscaled = Time::unscaledDeltaTime();
    if (!(unscaled >= 0.0 && unscaled <= 0.1) ||
        std::abs(Time::deltaTime() - unscaled * 2.0) > 1e-12 ||
        std::abs(Time::elapsedTime() - Time::deltaTime()) > 1e-12) return 6;

    Time::setFixedDeltaTime(1e-12);
    if (!Time::consumeFixedStep()) return 7;
    Time::setTimeScale(0.0);
    const double elapsed = Time::elapsedTime();
    Time::update();
    if (Time::deltaTime() != 0.0 || Time::elapsedTime() != elapsed) return 8;

    return 0;
}
