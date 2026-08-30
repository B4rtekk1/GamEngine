#pragma once

#include "Engine/Scene/ScenePresets.h"

#include <vector>

class ComponentsPanel final {
public:
    [[nodiscard]] static bool draw(Engine::ScenePreset& scene,
                                   const std::vector<Engine::Entity>& selection,
                                   Engine::Entity active,
                                   bool& isOpen);
};
