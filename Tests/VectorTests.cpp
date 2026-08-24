#include <Engine/Math/Vec2.h>
#include <Engine/Math/Vec4.h>

#include <cmath>

namespace {
bool near(const float a, const float b, const float epsilon = 0.0001f) {
    return std::abs(a - b) <= epsilon;
}
}

int main() {
    using namespace Engine;

    Vec2 a{3.0f, 4.0f};
    const Vec2 b{1.0f, -2.0f};
    if (!near(a.length(), 5.0f) || !near((a + b).x(), 4.0f) ||
        !near((a + b).y(), 2.0f) || !near((a - b).x(), 2.0f) ||
        !near((a * 2.0f).y(), 8.0f) || !near(a.normalized().length(), 1.0f)) return 1;
    a += b;
    if (!near(a.x(), 4.0f) || !near(a.y(), 2.0f)) return 2;
    a *= 0.5f;
    if (!near(a.x(), 2.0f) || !near(a.y(), 1.0f)) return 3;

    Vec4 value{1.0f, 2.0f, 3.0f, 4.0f};
    const Vec4 other{4.0f, 3.0f, 2.0f, 1.0f};
    if (!near((value + other).x(), 5.0f) || !near((value - other).w(), 3.0f) ||
        !near((value * other).z(), 6.0f) || !near((value / 2.0f).w(), 2.0f) ||
        !near(value.length(), std::sqrt(30.0f))) return 4;
    value.setX(5.0f);
    value.setW(-1.0f);
    if (value.x() != 5.0f || value.w() != -1.0f || !near(value.normalized().length(), 1.0f)) return 5;
    return 0;
}
