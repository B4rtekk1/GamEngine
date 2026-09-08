#pragma once

#include "Engine/Scene/ScenePresets.h"
#include "Engine/Assets/Content.h"
#include "Editor/ComponentDescriptor.h"

#include <vector>

class ComponentsPanel final {
public:
    [[nodiscard]] static bool draw(Engine::ScenePreset& scene,
                                   Engine::Assets::Content& content,
                                   const std::vector<Engine::Entity>& selection,
                                   Engine::Entity active,
                                   bool& isOpen);
};
