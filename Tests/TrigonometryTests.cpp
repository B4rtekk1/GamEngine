#include <Engine/Math/Trigonometry.h>

#include <cmath>
#include <numbers>

namespace {
bool near(const float left, const float right, const float epsilon = 0.0001F) {
    return std::abs(left - right) <= epsilon;
}
}

int main() {
    using namespace Engine;
    constexpr float pi = std::numbers::pi_v<float>;
    constexpr float halfPi = pi / 2.0F;
    constexpr float quarterPi = pi / 4.0F;

    if (!near(Engine::sin(0.0F), 0.0F) || !near(Engine::sin(Radians{halfPi}), 1.0F) ||
        !near(Engine::sin(Degrees{90.0F}), 1.0F) || !near(Engine::cos(0.0F), 1.0F) ||
        !near(Engine::cos(Radians{halfPi}), 0.0F) || !near(Engine::cos(Degrees{180.0F}), -1.0F)) return 1;
    if (!near(tan(Degrees{45.0F}), 1.0F) || !near(tan(Radians{0.0F}), 0.0F) ||
        !near(Engine::asin(1.0F).value(), halfPi) || !near(Engine::acos(0.0F).value(), halfPi) ||
        !near(Engine::atan(1.0F).value(), quarterPi) || !near(Engine::atan2(1.0F, 0.0F).value(), halfPi)) return 2;

    if (!near(cot(Degrees{45.0F}), 1.0F) || !near(sec(Degrees{0.0F}), 1.0F) ||
        !near(csc(Degrees{90.0F}), 1.0F) || !near(cot(Radians{quarterPi}), 1.0F) ||
        !near(sec(Radians{0.0F}), 1.0F) || !near(csc(Radians{halfPi}), 1.0F)) return 3;

    return 0;
}
