#pragma once

#include "Engine/Scene/ScenePresets.h"

#include <vector>

namespace Engine::Assets { class Content; }

class HierarchyPanel final {
public:
    enum class Action:uint8_t { None, Delete, Copy, Paste, Duplicate };

    [[nodiscard]] static Engine::Entity draw(Engine::ScenePreset& scene,
                                               Engine::Assets::Content& content,
                                               const std::vector<Engine::Entity>& selection,
                                               Action& action,
                                               Engine::Entity& actionEntity,
                                               bool canPaste,
                                               bool disabled,
                                               bool& isOpen);
};
