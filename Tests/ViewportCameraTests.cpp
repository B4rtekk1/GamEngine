#include <Engine/Renderer/ViewportCamera.h>

#include <cmath>

namespace {
bool near(const float left, const float right, const float epsilon = 0.001F) {
    return std::abs(left - right) <= epsilon;
}
}

int main() {
    using namespace Engine;

    CameraComponent component;
    component.fieldOfView = 75.0F;
    component.nearClip = 0.25F;
    component.farClip = 250.0F;
    Transform transform;
    transform.position = {4.0F, 5.0F, 6.0F};
    transform.rotation = {10.0F, -90.0F, 0.0F};

    const ViewportCamera game = ViewportCamera::game(component, transform, 2.0F);
    if (game.type != ViewportCameraType::Game ||
        !near(game.camera.position().x(), 4.0F) ||
        !near(game.camera.position().y(), 5.0F) ||
        !near(game.camera.position().z(), 6.0F) ||
        !near(game.camera.forward().x(), 0.0F) ||
        !near(game.camera.forward().y(), 0.173648F) ||
        !near(game.camera.forward().z(), -0.984808F) ||
        !(game.camera.projectionMatrix().native()[1][1] < 0.0F)) return 1;

    const ViewportCamera scene = ViewportCamera::scene(1.5F);
    if (scene.type != ViewportCameraType::Scene ||
        !near(scene.camera.position().x(), 8.0F) ||
        !near(scene.camera.position().y(), 6.0F) ||
        !near(scene.camera.position().z(), 8.0F) ||
        !near(scene.camera.forward().length(), 1.0F)) return 2;

    return 0;
}