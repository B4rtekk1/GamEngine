#pragma once

#include "Engine/Assets/Content.h"
#include "Engine/Scene/ScenePresets.h"

#include <filesystem>
#include <functional>

class AssetManagerPanel final {
  public:
    enum class Request : uint8_t { None, ImportFile, ImportFolder, ImportArchive, CreateShaderGraph };

    /** Queues an action that the panel runs on its next draw (used by the Assets menu). */
    static void request(Request request);

    /** Draws the asset browser and returns the entity created by Add to Scene. */
    [[nodiscard]] static Engine::Entity draw(Engine::ScenePreset& scene, Engine::Assets::Content& content,
                                             bool disabled, bool& isOpen, bool projectIsOpen,
                                             const std::function<void(const std::filesystem::path&)>& openShaderGraph,
                                             const std::function<void(const std::filesystem::path&)>& sceneDeleted);
};
