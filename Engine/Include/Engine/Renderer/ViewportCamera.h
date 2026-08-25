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
        Camera result{Degrees{60.0F}, aspectRatio, 0.1F, 1000.0F};
        result.setPosition(Vec3{8.0F, 6.0F, 8.0F});
        result.setRotation(Degrees{-135.0F}, Degrees{-22.0F});
        return {ViewportCameraType::Scene, result};
    }
};

} // namespace Engine
