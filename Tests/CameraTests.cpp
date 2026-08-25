#include <Engine/Core/Camera.h>

#include <cmath>
#include <stdexcept>

namespace {
bool near(const float left, const float right, const float epsilon = 0.001F) {
    return std::abs(left - right) <= epsilon;
}
}

int main() {
    using namespace Engine;

    Camera camera{Degrees{90.0F}, 16.0F / 9.0F, 0.1F, 100.0F};
    if (!near(camera.position().z(), 3.0F) || !near(camera.forward().x(), 0.0F) ||
        !near(camera.forward().y(), 0.0F) || !near(camera.forward().z(), -1.0F) ||
        !near(camera.right().x(), 1.0F) || !near(camera.right().y(), 0.0F) ||
        !near(camera.right().z(), 0.0F) || !near(camera.up().y(), 1.0F)) return 1;

    const Vec4 viewOrigin = camera.viewMatrix() * Vec4{0.0F, 0.0F, 0.0F, 1.0F};
    if (!near(viewOrigin.x(), 0.0F) || !near(viewOrigin.y(), 0.0F) ||
        !near(viewOrigin.z(), -3.0F) ||
        !(camera.projectionMatrix().native()[1][1] < 0.0F)) return 2;

    camera.setPosition({1.0F, 2.0F, 3.0F});
    camera.move({-1.0F, 0.5F, 2.0F});
    camera.setRotation(Degrees{0.0F}, Degrees{120.0F});
    if (!near(camera.position().x(), 0.0F) || !near(camera.position().y(), 2.5F) ||
        !near(camera.position().z(), 5.0F) || camera.forward().y() < 0.99F) return 3;

    camera.setRotation(Degrees{0.0F}, Degrees{-120.0F});
    if (camera.forward().y() > -0.99F) return 4;
    camera.setAspectRatio(1.0F);
    if (!(camera.projectionMatrix().native()[0][0] > 0.0F)) return 5;

    try {
        static_cast<void>(Camera{Degrees{0.0F}, 1.0F, 0.1F, 1.0F});
        return 6;
    } catch (const std::invalid_argument&) {
    }
    try {
        static_cast<void>(Camera{Degrees{90.0F}, 0.0F, 0.1F, 1.0F});
        return 7;
    } catch (const std::invalid_argument&) {
    }
    try {
        static_cast<void>(Camera{Degrees{90.0F}, 1.0F, 1.0F, 1.0F});
        return 8;
    } catch (const std::invalid_argument&) {
    }
    try {
        camera.setAspectRatio(0.0F);
        return 9;
    } catch (const std::invalid_argument&) {
    }

    return 0;
}