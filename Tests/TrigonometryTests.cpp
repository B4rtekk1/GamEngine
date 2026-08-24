#include <Engine/Math/Trigonometry.h>

#include <cmath>
#include <numbers>

namespace {
bool near(const float left, const float right, const float epsilon = 0.0001f) {
    return std::abs(left - right) <= epsilon;
}
}

int main() {
    using namespace Engine;
    constexpr float pi = std::numbers::pi_v<float>;
    constexpr float halfPi = pi / 2.0f;
    constexpr float quarterPi = pi / 4.0f;

    if (!near(sin(0.0f), 0.0f) || !near(sin(Radians{halfPi}), 1.0f) ||
        !near(sin(Degrees{90.0f}), 1.0f) || !near(cos(0.0f), 1.0f) ||
        !near(cos(Radians{halfPi}), 0.0f) || !near(cos(Degrees{180.0f}), -1.0f)) return 1;
    if (!near(tan(Degrees{45.0f}), 1.0f) || !near(tan(Radians{0.0f}), 0.0f) ||
        !near(asin(1.0f).value(), halfPi) || !near(acos(0.0f).value(), halfPi) ||
        !near(atan(1.0f).value(), quarterPi) || !near(atan2(1.0f, 0.0f).value(), halfPi)) return 2;

    if (!near(cot(Degrees{45.0f}), 1.0f) || !near(sec(Degrees{0.0f}), 1.0f) ||
        !near(csc(Degrees{90.0f}), 1.0f) || !near(cot(Radians{quarterPi}), 1.0f) ||
        !near(sec(Radians{0.0f}), 1.0f) || !near(csc(Radians{halfPi}), 1.0f)) return 3;

    return 0;
}
