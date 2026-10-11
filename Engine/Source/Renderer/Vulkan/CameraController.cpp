#include "Engine/Renderer/Vulkan/CameraController.h"

#include "Engine/Core/Time.h"
#include "Engine/Core/Transform.h"
#include "Engine/Core/SceneCamera.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/ECS/Registry.h"
#include "Engine/Input/Input.h"

#include <algorithm>

namespace Engine {
    namespace {
        constexpr float Zero = 0.0F;
        constexpr float MovementSpeed = 10.0F;
        constexpr float MouseWheelSpeed = 2.0F;
        constexpr float MouseSensitivity = 0.25F;
        constexpr float MaxPitchDegrees = 89.0F;

        constexpr float EditorCameraFovDegrees = 60.0F;
        constexpr float EditorCameraAspectRatio = 1.0F;
        constexpr float EditorCameraNearClip = 0.1F;
        constexpr float EditorCameraFarClip = 1000.0F;
        constexpr float EditorPanSpeed = 0.03F;
        constexpr float EditorFastMovementMultiplier = 4.0F;
    } // namespace

    void CameraController::update(Registry &registry) {
        if (editorInputEnabled_) {
            updateEditor();
            return;
        }
        if (!gameInputEnabled_) {
            return;
        }
        CameraComponent *activeCamera = nullptr;
        Transform *activeTransform = nullptr;
        Entity activeCameraEntity = NullEntity;
        registry.view<CameraComponent, Transform>(
            [&](const Entity entity, CameraComponent &component, Transform &transform) {
                if (!activeCamera && component.primary) {
                    activeCamera = &component;
                    activeTransform = &transform;
                    activeCameraEntity = entity;
                }
            });

        if (activeCamera == nullptr) {
            camera_.reset();
            return;
        }

        // Commit through Registry so transform and scene-frame caches observe
        // camera motion instead of retaining the previous view indefinitely.
        Transform transform = *activeTransform;
        camera_ = makeSceneCamera(registry, activeCameraEntity);

        const bool flyMode = Input::cursorMode() == CursorMode::Locked;
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
        Vec3 worldTranslation{};
        if (movement.length() > Zero) {
            worldTranslation += movement.normalized() *
            (MovementSpeed * (moveFast ? EditorFastMovementMultiplier : 1.0F) *
             static_cast<float>(Time::deltaTime()));
        }
        if (flyMode) {
            worldTranslation += camera_->forward() * (Input::mouseWheel() * MouseWheelSpeed);
        }
        Vec3 localTranslation = worldTranslation;
        if (registry.has<ParentComponent>(activeCameraEntity)) {
            const auto parent = registry.get<ParentComponent>(activeCameraEntity).runtimeParent;
            if (registry.valid(parent) && registry.has<Transform>(parent)) {
                const auto& parentMatrix = registry.get<Transform>(parent).worldMatrix().native();
                if (std::abs(glm::determinant(parentMatrix)) > 1.0e-6F)
                    localTranslation = Vec3{glm::inverse(parentMatrix) * glm::vec4{worldTranslation.native(), 0.0F}};
            }
        }
        transform.position += localTranslation;

        if (flyMode) {
            const Vec2 delta = Input::mouseDelta();
            transform.rotation.setY(transform.rotation.y() + (delta.x() * MouseSensitivity));
            transform.rotation.setX(std::clamp(transform.rotation.x() - (delta.y() * MouseSensitivity),
                                               -MaxPitchDegrees, MaxPitchDegrees));
        }
        if (transform.position.native() != activeTransform->position.native() ||
            transform.rotation.native() != activeTransform->rotation.native()) {
            registry.modify<Transform>(activeCameraEntity, [&](Transform& destination) {
                destination.position = transform.position;
                destination.rotation = transform.rotation;
            });
        }
        camera_ = makeSceneCamera(registry, activeCameraEntity);
    }

    void CameraController::updateEditor() {
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
