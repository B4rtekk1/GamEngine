#pragma once

#include "Engine/Assets/Content.h"
#include "Engine/Scene/ScenePresets.h"

#include <filesystem>
#include <functional>

class AssetManagerPanel final {
public:
    /** Draws the asset browser and returns the entity created by Add to Scene. */
    [[nodiscard]] static Engine::Entity draw(Engine::ScenePreset& scene,
                                               Engine::Assets::Content& content,
                                               bool disabled,
                                               bool& isOpen,
                                               bool projectIsOpen,
                                               const std::function<void(const std::filesystem::path&)>& openShaderGraph,
                                               const std::function<void(const std::filesystem::path&)>& sceneDeleted);
};
