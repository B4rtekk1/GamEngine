#include <Engine/ECS/Components/CameraComponent.h>

#include <cmath>

namespace {
bool near(const float left, const float right, const float epsilon = 0.0001f) {
    return std::abs(left - right) <= epsilon;
}
}

int main() {
    using namespace Engine;

    CameraComponent camera;
    if (!camera.isPerspective() || camera.isOrthographic() || !camera.isValid() ||
        !near(camera.fieldOfView, 60.0f) || !near(camera.aspectRatio, 16.0f / 9.0f)) return 1;

    camera.setOrthographic(-2.0f, -1.0f, -3.0f);
    if (!camera.isOrthographic() || camera.isPerspective() ||
        !near(camera.orthographicSize, 0.0001f) || !near(camera.nearClip, 0.0001f) ||
        !(camera.farClip > camera.nearClip) || !camera.isValid()) return 2;

    camera.setPerspective(-5.0f, 0.0f, 0.0f);
    if (!camera.isPerspective() || !near(camera.fieldOfView, 1.0f) ||
        !near(camera.nearClip, 0.0001f) || !(camera.farClip > camera.nearClip) ||
        !camera.isValid()) return 3;

    camera.setPerspective(200.0f, 2.0f, 10.0f);
    if (!near(camera.fieldOfView, 179.0f) || !near(camera.nearClip, 2.0f) ||
        !near(camera.farClip, 10.0f)) return 4;

    camera.setAspectRatio(1920.0f, 1080.0f);
    if (!near(camera.aspectRatio, 16.0f / 9.0f)) return 5;
    camera.setAspectRatio(0.0f, 1080.0f);
    camera.setAspectRatio(1920.0f, 0.0f);
    if (!near(camera.aspectRatio, 16.0f / 9.0f)) return 6;
    camera.setAspectRatio(-1.0f);
    if (!near(camera.aspectRatio, 16.0f / 9.0f)) return 7;
    camera.setAspectRatio(2.0f);
    if (!near(camera.aspectRatio, 2.0f)) return 8;

    camera.fieldOfView = 180.0f;
    if (camera.isValid()) return 9;
    camera.fieldOfView = 90.0f;
    camera.nearClip = 5.0f;
    camera.farClip = 4.0f;
    if (camera.isValid()) return 10;

    return 0;
}
