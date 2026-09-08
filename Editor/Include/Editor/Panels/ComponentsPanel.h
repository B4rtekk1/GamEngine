#pragma once

#include "Engine/Scene/ScenePresets.h"
#include "Engine/Assets/Content.h"
#include "Editor/ComponentDescriptor.h"

#include <filesystem>
#include <vector>

class ComponentsPanel final {
public:
    [[nodiscard]] static bool draw(Engine::ScenePreset& scene,
                                   Engine::Assets::Content& content,
                                   const std::filesystem::path& shaderSourceDirectory,
                                   const std::vector<Engine::Entity>& selection,
                                   Engine::Entity active,
                                   bool& isOpen);
};
