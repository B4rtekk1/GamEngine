#pragma once

#include "Engine/Core/Camera.h"
#include "Engine/Core/Transform.h"
#include "Engine/ECS/Components/CameraComponent.h"

namespace Engine {

enum class ViewportCameraType { Game, Scene };

struct ViewportCamera final {
    ViewportCameraType type;
    Camera camera;

    static ViewportCamera game(const CameraComponent& component, const Transform& transform,
                               float aspectRatio) {
        Camera result{Degrees{component.fieldOfView}, aspectRatio,
                      component.nearClip, component.farClip};
        result.setPosition(transform.position);
        result.setRotation(Degrees{transform.rotation.y()}, Degrees{transform.rotation.x()});
        return {ViewportCameraType::Game, result};
    }

    static ViewportCamera scene(float aspectRatio) {
        Camera result{Degrees{60.0f}, aspectRatio, 0.1f, 1000.0f};
        result.setPosition(Vec3{8.0f, 6.0f, 8.0f});
        result.setRotation(Degrees{-135.0f}, Degrees{-22.0f});
        return {ViewportCameraType::Scene, result};
    }
};

} // namespace Engine
