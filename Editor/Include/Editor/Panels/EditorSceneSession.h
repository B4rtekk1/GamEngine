#pragma once

#include "Engine/Renderer/RenderConfig.h"
#include "Engine/Scene/ScenePresets.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Engine { class Renderer; }

class EditorSceneSession final {
public:
    /** Sets the root directory used for generated project files. */
    static void setProjectRoot(std::filesystem::path path);
    /** Selects the scene used by Save and Load actions. An empty path clears the selection. */
    static void setScenePath(std::filesystem::path path);
    [[nodiscard]] static std::filesystem::path scenePath();
    [[nodiscard]] static bool hasSavedScene();
    /** Opens the platform Save As dialog for a scene file. */
    [[nodiscard]] static std::optional<std::filesystem::path> chooseSaveScenePath();
    /** Opens the platform file picker for an existing scene file. */
    [[nodiscard]] static std::optional<std::filesystem::path> chooseLoadScenePath();
    /** Opens the platform file picker for a GamEngine project manifest. */
    [[nodiscard]] static std::optional<std::filesystem::path> chooseLoadProjectPath();
    /** Marks the current scene as persisted at @p path. */
    static void markSceneSaved(std::filesystem::path path);
    /** Clears the persisted-scene association after its file was removed. */
    static void clearSavedScene();
    [[nodiscard]] static Engine::AntialiasingLevel antialiasingLevel(const Engine::Renderer& renderer);

    static bool setPlayMode(bool play, Engine::ScenePreset& scene, std::string& snapshot,
                            std::string& error, Engine::AntialiasingLevel antialiasing);
    static bool createCppScript(std::string_view name, std::string& error);
};
