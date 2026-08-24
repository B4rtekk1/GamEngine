#include <Engine/Renderer/ViewportCamera.h>

#include <cmath>

namespace {
bool near(const float left, const float right, const float epsilon = 0.001f) {
    return std::abs(left - right) <= epsilon;
}
}

int main() {
    using namespace Engine;

    CameraComponent component;
    component.fieldOfView = 75.0f;
    component.nearClip = 0.25f;
    component.farClip = 250.0f;
    Transform transform;
    transform.position = {4.0f, 5.0f, 6.0f};
    transform.rotation = {10.0f, -90.0f, 0.0f};

    const ViewportCamera game = ViewportCamera::game(component, transform, 2.0f);
    if (game.type != ViewportCameraType::Game ||
        !near(game.camera.position().x(), 4.0f) ||
        !near(game.camera.position().y(), 5.0f) ||
        !near(game.camera.position().z(), 6.0f) ||
        !near(game.camera.forward().x(), 0.0f) ||
        !near(game.camera.forward().y(), 0.173648f) ||
        !near(game.camera.forward().z(), -0.984808f) ||
        !(game.camera.projectionMatrix().native()[1][1] < 0.0f)) return 1;

    const ViewportCamera scene = ViewportCamera::scene(1.5f);
    if (scene.type != ViewportCameraType::Scene ||
        !near(scene.camera.position().x(), 8.0f) ||
        !near(scene.camera.position().y(), 6.0f) ||
        !near(scene.camera.position().z(), 8.0f) ||
        !near(scene.camera.forward().length(), 1.0f)) return 2;

    return 0;
}
