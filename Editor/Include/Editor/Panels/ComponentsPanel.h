#pragma once

#include "Engine/Scene/ScenePresets.h"

class ComponentsPanel final {
public:
    [[nodiscard]] static bool draw(Engine::ScenePreset& scene,
                                   Engine::Entity selected,
                                   bool& isOpen);
};
