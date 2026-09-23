#pragma once

#include "Engine/Core/Camera.h"

#include <optional>

namespace Engine {
//NOLINTBEGIN
class Registry;

/** Owns gameplay and editor Scene View camera state and input handling. */
class CameraController final {
public:
    void setEditorInputEnabled(bool enabled) noexcept { editorInputEnabled_ = enabled; }
    [[nodiscard]] bool editorInputEnabled() const noexcept { return editorInputEnabled_; }
    void setGameInputEnabled(bool enabled) noexcept { gameInputEnabled_ = enabled; }
    [[nodiscard]] bool gameInputEnabled() const noexcept { return gameInputEnabled_; }
    void update(Registry& registry);
    void updateEditor();

    [[nodiscard]] const std::optional<Camera>& camera() const noexcept { return camera_; }
    [[nodiscard]] std::optional<Camera>& camera() noexcept { return camera_; }
    [[nodiscard]] Vec3 editorPosition() const noexcept { return editorPosition_; }
    [[nodiscard]] float editorYaw() const noexcept { return editorYaw_; }
    [[nodiscard]] float editorPitch() const noexcept { return editorPitch_; }
    void setEditorPosition(const Vec3& position) noexcept { editorPosition_ = position; }
    void setEditorRotation(const float yaw, const float pitch) noexcept {
        editorYaw_ = yaw;
        editorPitch_ = pitch;
    }

private:
    std::optional<Camera> camera_;
    bool editorInputEnabled_{false};
    bool gameInputEnabled_{false};
    Vec3 editorPosition_{8.0F, 6.0F, 8.0F};
    float editorYaw_{-135.0F};
    float editorPitch_{-28.0F};
};

} // namespace Engine
//NOLINTEND
