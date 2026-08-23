#pragma once

#include "Engine/Scene/ScenePresets.h"

class HierarchyPanel final {
public:
    enum class Action { None, Delete, Duplicate };

    [[nodiscard]] static Engine::Entity draw(Engine::ScenePreset& scene,
                                               Engine::Entity selected,
                                               Action& action,
                                               Engine::Entity& actionEntity);
};
