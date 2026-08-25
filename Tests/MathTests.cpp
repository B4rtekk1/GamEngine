#include <Engine/Math/Math.h>

#include <cmath>
#include <stdexcept>

namespace {
bool near(const float a, const float b, const float epsilon = 0.0001F) {
    return std::abs(a - b) <= epsilon;
}
}

int main() {
    using namespace Engine;

    const Vec3 first{1.0F, 2.0F, 3.0F};
    const Vec3 second{-2.0F, 4.0F, 1.0F};
    const Vec3 crossProduct = cross(first, second);
    if (!near((first + second).x(), -1.0F) || !near((first - second).y(), -2.0F) ||
        !near(dot(first, second), 9.0F) ||
        !near(crossProduct.x(), -10.0F) || !near(crossProduct.y(), -7.0F) ||
        !near(crossProduct.z(), 8.0F) || !near(first.length(), std::sqrt(14.0F)) ||
        !near(first.normalized().length(), 1.0F)) return 2;

    const Degrees ninetyDegrees{90.0F};
    const Radians ninetyRadians = ninetyDegrees.toRadians();
    if (!near(ninetyRadians.value(), HalfPi) || !near(ninetyRadians.toDegrees().value(), 90.0F)) return 3;
    if (!near((Degrees{30.0F} + Degrees{15.0F}).value(), 45.0F) ||
        !near((Radians{2.0F} * 3.0F).value(), 6.0F)) return 4;

    const Quat quarterTurn = Quat::angleAxis(HalfPi, {0.0F, 0.0F, 1.0F});
    const Vec3 rotated = quarterTurn * Vec3{1.0F, 0.0F, 0.0F};
    if (!near(rotated.x(), 0.0F) || !near(rotated.y(), 1.0F) || !near(rotated.z(), 0.0F)) return 5;

    const Mat4 translation = Mat4::translate({3.0F, 4.0F, 5.0F});
    const Vec4 translated = translation * Vec4{1.0F, 2.0F, 3.0F, 1.0F};
    if (!near(translated.x(), 4.0F) || !near(translated.y(), 6.0F) ||
        !near(translated.z(), 8.0F) || !near(translated.w(), 1.0F)) return 6;

    const AABB bounds = AABB::unitCube().transformed(translation.native());
    if (!near(bounds.min.x(), 2.5F) || !near(bounds.min.y(), 3.5F) ||
        !near(bounds.min.z(), 4.5F) || !near(bounds.max.x(), 3.5F) ||
        !near(bounds.max.y(), 4.5F) || !near(bounds.max.z(), 5.5F)) return 7;

    const Color color = Color::from_hex("#3366CC80");
    if (!near(color.r(), 0x33 / 255.0F) || !near(color.g(), 0x66 / 255.0F) ||
        !near(color.b(), 0xCC / 255.0F) || !near(color.a(), 0x80 / 255.0F) ||
        Color::from_hex("ffffff").a() != 1.0F) return 8;
    if (Color::lerp(Color::black(), Color::white(), 0.5F).r() != 0.5F ||
        Color{2.0F, -1.0F, 0.5F, 3.0F}.clamped().g() != 0.0F) return 9;

    try {
        static_cast<void>(Color::from_hex("xyz"));
        return 10;
    } catch (const std::invalid_argument&) {
    }

    return 0;
}