#include <Engine/ECS/Components/CameraComponent.h>

#include <cmath>

namespace {
bool near(const float left, const float right, const float epsilon = 0.0001F) {
    return std::abs(left - right) <= epsilon;
}
}

int main() {
    using namespace Engine;

    CameraComponent camera;
    if (!camera.isPerspective() || camera.isOrthographic() || !camera.isValid() ||
        !near(camera.fieldOfView, 60.0F) || !near(camera.aspectRatio, 16.0F / 9.0F)) return 1;

    camera.setOrthographic(-2.0F, -1.0F, -3.0F);
    if (!camera.isOrthographic() || camera.isPerspective() ||
        !near(camera.orthographicSize, 0.0001F) || !near(camera.nearClip, 0.0001F) ||
        !(camera.farClip > camera.nearClip) || !camera.isValid()) return 2;

    camera.setPerspective(-5.0F, 0.0F, 0.0F);
    if (!camera.isPerspective() || !near(camera.fieldOfView, 1.0F) ||
        !near(camera.nearClip, 0.0001F) || !(camera.farClip > camera.nearClip) ||
        !camera.isValid()) return 3;

    camera.setPerspective(200.0F, 2.0F, 10.0F);
    if (!near(camera.fieldOfView, 179.0F) || !near(camera.nearClip, 2.0F) ||
        !near(camera.farClip, 10.0F)) return 4;

    camera.setAspectRatio(1920.0F, 1080.0F);
    if (!near(camera.aspectRatio, 16.0F / 9.0F)) return 5;
    camera.setAspectRatio(0.0F, 1080.0F);
    camera.setAspectRatio(1920.0F, 0.0F);
    if (!near(camera.aspectRatio, 16.0F / 9.0F)) return 6;
    camera.setAspectRatio(-1.0F);
    if (!near(camera.aspectRatio, 16.0F / 9.0F)) return 7;
    camera.setAspectRatio(2.0F);
    if (!near(camera.aspectRatio, 2.0F)) return 8;

    camera.fieldOfView = 180.0F;
    if (camera.isValid()) return 9;
    camera.fieldOfView = 90.0F;
    camera.nearClip = 5.0F;
    camera.farClip = 4.0F;
    if (camera.isValid()) return 10;

    return 0;
}