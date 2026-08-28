#pragma once

#include "Engine/Assets/Content.h"
#include "Engine/Scene/ScenePresets.h"

class AssetManagerPanel final {
public:
    /** Draws the asset browser and returns the entity created by Add to Scene. */
    [[nodiscard]] static Engine::Entity draw(Engine::ScenePreset& scene,
                                               Engine::Assets::Content& content,
                                               bool disabled,
                                               bool& isOpen);
};
