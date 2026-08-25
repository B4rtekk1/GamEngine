#include <Engine/Math/Vec2.h>
#include <Engine/Math/Vec4.h>

#include <cmath>

namespace {
bool near(const float a, const float b, const float epsilon = 0.0001F) {
    return std::abs(a - b) <= epsilon;
}
}

int main() {
    using namespace Engine;

    Vec2 a{3.0F, 4.0F};
    const Vec2 b{1.0F, -2.0F};
    if (!near(a.length(), 5.0F) || !near((a + b).x(), 4.0F) ||
        !near((a + b).y(), 2.0F) || !near((a - b).x(), 2.0F) ||
        !near((a * 2.0F).y(), 8.0F) || !near(a.normalized().length(), 1.0F)) return 1;
    a += b;
    if (!near(a.x(), 4.0F) || !near(a.y(), 2.0F)) return 2;
    a *= 0.5F;
    if (!near(a.x(), 2.0F) || !near(a.y(), 1.0F)) return 3;

    Vec4 value{1.0F, 2.0F, 3.0F, 4.0F};
    const Vec4 other{4.0F, 3.0F, 2.0F, 1.0F};
    if (!near((value + other).x(), 5.0F) || !near((value - other).w(), 3.0F) ||
        !near((value * other).z(), 6.0F) || !near((value / 2.0F).w(), 2.0F) ||
        !near(value.length(), std::sqrt(30.0F))) return 4;
    value.setX(5.0F);
    value.setW(-1.0F);
    if (value.x() != 5.0F || value.w() != -1.0F || !near(value.normalized().length(), 1.0F)) return 5;
    return 0;
}