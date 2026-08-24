#include <Engine/Core/Camera.h>

#include <cmath>
#include <stdexcept>

namespace {
bool near(const float left, const float right, const float epsilon = 0.001f) {
    return std::abs(left - right) <= epsilon;
}
}

int main() {
    using namespace Engine;

    Camera camera{Degrees{90.0f}, 16.0f / 9.0f, 0.1f, 100.0f};
    if (!near(camera.position().z(), 3.0f) || !near(camera.forward().x(), 0.0f) ||
        !near(camera.forward().y(), 0.0f) || !near(camera.forward().z(), -1.0f) ||
        !near(camera.right().x(), 1.0f) || !near(camera.right().y(), 0.0f) ||
        !near(camera.right().z(), 0.0f) || !near(camera.up().y(), 1.0f)) return 1;

    const Vec4 viewOrigin = camera.viewMatrix() * Vec4{0.0f, 0.0f, 0.0f, 1.0f};
    if (!near(viewOrigin.x(), 0.0f) || !near(viewOrigin.y(), 0.0f) ||
        !near(viewOrigin.z(), -3.0f) ||
        !(camera.projectionMatrix().native()[1][1] < 0.0f)) return 2;

    camera.setPosition({1.0f, 2.0f, 3.0f});
    camera.move({-1.0f, 0.5f, 2.0f});
    camera.setRotation(Degrees{0.0f}, Degrees{120.0f});
    if (!near(camera.position().x(), 0.0f) || !near(camera.position().y(), 2.5f) ||
        !near(camera.position().z(), 5.0f) || camera.forward().y() < 0.99f) return 3;

    camera.setRotation(Degrees{0.0f}, Degrees{-120.0f});
    if (camera.forward().y() > -0.99f) return 4;
    camera.setAspectRatio(1.0f);
    if (!(camera.projectionMatrix().native()[0][0] > 0.0f)) return 5;

    try {
        static_cast<void>(Camera{Degrees{0.0f}, 1.0f, 0.1f, 1.0f});
        return 6;
    } catch (const std::invalid_argument&) {
    }
    try {
        static_cast<void>(Camera{Degrees{90.0f}, 0.0f, 0.1f, 1.0f});
        return 7;
    } catch (const std::invalid_argument&) {
    }
    try {
        static_cast<void>(Camera{Degrees{90.0f}, 1.0f, 1.0f, 1.0f});
        return 8;
    } catch (const std::invalid_argument&) {
    }
    try {
        camera.setAspectRatio(0.0f);
        return 9;
    } catch (const std::invalid_argument&) {
    }

    return 0;
}
