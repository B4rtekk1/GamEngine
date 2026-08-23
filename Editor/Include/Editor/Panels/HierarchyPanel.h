#pragma once

#include "Engine/Scene/ScenePresets.h"

class HierarchyPanel final {
public:
    [[nodiscard]] static Engine::Entity draw(Engine::ScenePreset& scene,
                                               Engine::Entity selected);
};
