#pragma once

#include "Engine/Core/Camera.h"

#include <SDL3/SDL.h>

#include <optional>

namespace Engine {

class Registry;

/** Owns gameplay and editor Scene View camera state and input handling. */
class CameraController final {
public:
    void setEditorInputEnabled(bool enabled) noexcept { editorInputEnabled_ = enabled; }
    [[nodiscard]] bool editorInputEnabled() const noexcept { return editorInputEnabled_; }
    void update(SDL_Window* window, Registry& registry);
    void updateEditor(SDL_Window* window);

    [[nodiscard]] const std::optional<Camera>& camera() const noexcept { return camera_; }
    [[nodiscard]] std::optional<Camera>& camera() noexcept { return camera_; }
    [[nodiscard]] Vec3 editorPosition() const noexcept { return editorPosition_; }
    [[nodiscard]] float editorYaw() const noexcept { return editorYaw_; }
    [[nodiscard]] float editorPitch() const noexcept { return editorPitch_; }
    void setEditorPosition(const Vec3& position) noexcept { editorPosition_ = position; }
    void setEditorRotation(float yaw, float pitch) noexcept {
        editorYaw_ = yaw;
        editorPitch_ = pitch;
    }

private:
    void disableRelativeMouseMode(SDL_Window* window);

    std::optional<Camera> camera_;
    bool mouseLookActive_{false};
    bool editorInputEnabled_{false};
    Vec3 editorPosition_{8.0f, 6.0f, 8.0f};
    float editorYaw_{-135.0f};
    float editorPitch_{-28.0f};
};

} // namespace Engine
