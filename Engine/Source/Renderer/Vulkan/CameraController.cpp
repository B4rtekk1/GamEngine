#include "Engine/Renderer/Vulkan/CameraController.h"

#include "Engine/Core/Time.h"
#include "Engine/Core/Transform.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/ECS/Registry.h"
#include "Engine/Input/Input.h"
#include "Platform/SDL/SDLInput.h"

#include <algorithm>

namespace Engine {
    namespace {
        constexpr float Zero = 0.0F;
        constexpr float MovementSpeed = 10.0F;
        constexpr float MouseWheelSpeed = 2.0F;
        constexpr float MouseSensitivity = 0.1F;
        constexpr float MaxPitchDegrees = 89.0F;

        constexpr float EditorCameraFovDegrees = 60.0F;
        constexpr float EditorCameraAspectRatio = 1.0F;
        constexpr float EditorCameraNearClip = 0.1F;
        constexpr float EditorCameraFarClip = 1000.0F;
        constexpr float EditorPanSpeed = 0.03F;
        constexpr float EditorFastMovementMultiplier = 4.0F;
    } // namespace

    void CameraController::disableRelativeMouseMode(SDL_Window *window) {
        if (!mouseLookActive_) {
            return;
        }
        SDLInput::setRelativeMouseMode(window, false);
        mouseLookActive_ = false;
    }

    void CameraController::update(SDL_Window *window, Registry &registry) {
        if (editorInputEnabled_) {
            updateEditor(window);
            return;
        }
        if (!gameInputEnabled_) {
            disableRelativeMouseMode(window);
            return;
        }

        CameraComponent *activeCamera = nullptr;
        Transform *activeTransform = nullptr;
        registry.view<CameraComponent, Transform>(
            [&](const Entity, CameraComponent &component, Transform &transform) {
                if (!activeCamera && component.primary) {
                    activeCamera = &component;
                    activeTransform = &transform;
                }
            });

        if (activeCamera == nullptr) {
            disableRelativeMouseMode(window);
            camera_.reset();
            return;
        }

        const CameraComponent &component = *activeCamera;
        Transform &transform = *activeTransform;
        camera_.emplace(Degrees{component.fieldOfView}, component.aspectRatio,
                        component.nearClip, component.farClip);
        camera_->setRotation(Degrees{transform.rotation.y()}, Degrees{transform.rotation.x()});

        const bool flyMode = Input::mouseDown(MouseButton::Right);
        const bool moveFast = Input::keyDown(KeyCode::LeftShift) ||
                              Input::keyDown(KeyCode::RightShift);
        Vec3 movement{};
        const Vec3 forward = camera_->forward();
        const Vec3 horizontalForward{forward.x(), Zero, forward.z()};
        if (flyMode && horizontalForward.length() > Zero) {
            const Vec3 flatForward = horizontalForward.normalized();
            if (Input::keyDown(KeyCode::W)) {
                movement += flatForward;
            }
            if (Input::keyDown(KeyCode::S)) {
                movement -= flatForward;
            }
        }
        if (flyMode && Input::keyDown(KeyCode::D)) {
            movement += camera_->right();
        }
        if (flyMode && Input::keyDown(KeyCode::A)) {
            movement -= camera_->right();
        }
        if (flyMode && Input::keyDown(KeyCode::E)) {
            movement += camera_->up();
        }
        if (flyMode && Input::keyDown(KeyCode::Q)) {
            movement -= camera_->up();
        }
        if (movement.length() > Zero) {
            transform.position += movement.normalized() *
                    (MovementSpeed * (moveFast ? EditorFastMovementMultiplier : 1.0F) *
                     static_cast<float>(Time::deltaTime()));
        }
        if (flyMode) {
            transform.position += camera_->forward() * (Input::mouseWheel() * MouseWheelSpeed);
        }

        if (flyMode) {
            if (!mouseLookActive_) {
                SDLInput::setRelativeMouseMode(window, true);
                mouseLookActive_ = true;
            }
            const Vec2 delta = Input::mouseDelta();
            transform.rotation.setY(transform.rotation.y() + (delta.x() * MouseSensitivity));
            transform.rotation.setX(std::clamp(transform.rotation.x() - (delta.y() * MouseSensitivity),
                                               -MaxPitchDegrees, MaxPitchDegrees));
        } else {
            disableRelativeMouseMode(window);
        }
    }

    void CameraController::updateEditor(SDL_Window *window) {
        disableRelativeMouseMode(window);
        Camera sceneCamera{
            Degrees{EditorCameraFovDegrees}, EditorCameraAspectRatio,
            EditorCameraNearClip, EditorCameraFarClip
        };
        sceneCamera.setRotation(Degrees{editorYaw_}, Degrees{editorPitch_});

        // Scene View navigation deliberately has an explicit activation
        // gesture.  Without it, merely hovering the viewport made WASD move
        // the camera and clashed with the editor's transform shortcuts.
        const bool flyMode = Input::mouseDown(MouseButton::Right);
        const bool moveFast = Input::keyDown(KeyCode::LeftShift) ||
                              Input::keyDown(KeyCode::RightShift);
        Vec3 movement{};
        const Vec3 forward = sceneCamera.forward();
        const Vec3 horizontalForward{forward.x(), Zero, forward.z()};
        if (flyMode && horizontalForward.length() > Zero) {
            const Vec3 flatForward = horizontalForward.normalized();
            if (Input::keyDown(KeyCode::W)) { movement += flatForward; }
            if (Input::keyDown(KeyCode::S)) { movement -= flatForward; }
        }
        if (flyMode && Input::keyDown(KeyCode::D)) { movement += sceneCamera.right(); }
        if (flyMode && Input::keyDown(KeyCode::A)) { movement -= sceneCamera.right(); }
        if (flyMode && Input::keyDown(KeyCode::E)) { movement += sceneCamera.up(); }
        if (flyMode && Input::keyDown(KeyCode::Q)) { movement -= sceneCamera.up(); }
        if (Input::mouseDown(MouseButton::Middle)) {
            const Vec2 delta = Input::mouseDelta();
            editorPosition_ -= sceneCamera.right() * (delta.x() * EditorPanSpeed);
            editorPosition_ += sceneCamera.up() * (delta.y() * EditorPanSpeed);
        }
        if (movement.length() > Zero) {
            const float speed = MovementSpeed * (moveFast ? EditorFastMovementMultiplier : 1.0F);
            editorPosition_ += movement.normalized() *
                    (speed * static_cast<float>(Time::deltaTime()));
        }
        editorPosition_ += sceneCamera.forward() * (Input::mouseWheel() * MouseWheelSpeed);
        if (flyMode) {
            const Vec2 delta = Input::mouseDelta();
            editorYaw_ += (delta.x() * MouseSensitivity);
            editorPitch_ = std::clamp(editorPitch_ - (delta.y() * MouseSensitivity),
                                      -MaxPitchDegrees, MaxPitchDegrees);
        }
    }
} // namespace Engine
