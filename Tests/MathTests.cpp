#include <Engine/Math/Math.h>

#include <cmath>
#include <stdexcept>

namespace {
bool near(const float a, const float b, const float epsilon = 0.0001f) {
    return std::abs(a - b) <= epsilon;
}
}

int main() {
    using namespace Engine;

    const Vec3 first{1.0f, 2.0f, 3.0f};
    const Vec3 second{-2.0f, 4.0f, 1.0f};
    const Vec3 crossProduct = cross(first, second);
    if (!near((first + second).x(), -1.0f) || !near((first - second).y(), -2.0f) ||
        !near(dot(first, second), 9.0f) ||
        !near(crossProduct.x(), -10.0f) || !near(crossProduct.y(), -7.0f) ||
        !near(crossProduct.z(), 8.0f) || !near(first.length(), std::sqrt(14.0f)) ||
        !near(first.normalized().length(), 1.0f)) return 2;

    const Degrees ninetyDegrees{90.0f};
    const Radians ninetyRadians = ninetyDegrees.toRadians();
    if (!near(ninetyRadians.value(), HalfPi) || !near(ninetyRadians.toDegrees().value(), 90.0f)) return 3;
    if (!near((Degrees{30.0f} + Degrees{15.0f}).value(), 45.0f) ||
        !near((Radians{2.0f} * 3.0f).value(), 6.0f)) return 4;

    const Quat quarterTurn = Quat::angleAxis(HalfPi, {0.0f, 0.0f, 1.0f});
    const Vec3 rotated = quarterTurn * Vec3{1.0f, 0.0f, 0.0f};
    if (!near(rotated.x(), 0.0f) || !near(rotated.y(), 1.0f) || !near(rotated.z(), 0.0f)) return 5;

    const Mat4 translation = Mat4::translate({3.0f, 4.0f, 5.0f});
    const Vec4 translated = translation * Vec4{1.0f, 2.0f, 3.0f, 1.0f};
    if (!near(translated.x(), 4.0f) || !near(translated.y(), 6.0f) ||
        !near(translated.z(), 8.0f) || !near(translated.w(), 1.0f)) return 6;

    const AABB bounds = AABB::unitCube().transformed(translation.native());
    if (!near(bounds.min.x(), 2.5f) || !near(bounds.min.y(), 3.5f) ||
        !near(bounds.min.z(), 4.5f) || !near(bounds.max.x(), 3.5f) ||
        !near(bounds.max.y(), 4.5f) || !near(bounds.max.z(), 5.5f)) return 7;

    const Color color = Color::from_hex("#3366CC80");
    if (!near(color.r(), 0x33 / 255.0f) || !near(color.g(), 0x66 / 255.0f) ||
        !near(color.b(), 0xCC / 255.0f) || !near(color.a(), 0x80 / 255.0f) ||
        Color::from_hex("ffffff").a() != 1.0f) return 8;
    if (Color::lerp(Color::black(), Color::white(), 0.5f).r() != 0.5f ||
        Color{2.0f, -1.0f, 0.5f, 3.0f}.clamped().g() != 0.0f) return 9;

    try {
        static_cast<void>(Color::from_hex("xyz"));
        return 10;
    } catch (const std::invalid_argument&) {
    }

    return 0;
}
