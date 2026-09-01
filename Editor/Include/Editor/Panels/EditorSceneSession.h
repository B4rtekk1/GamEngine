#pragma once

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
    /** Sets the project startup scene used by the Save and Load menu actions. */
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
    [[nodiscard]] static std::uint32_t msaaSampleCount(const Engine::Renderer& renderer);

    static bool setPlayMode(bool play, Engine::ScenePreset& scene, std::string& snapshot,
                            std::string& error, std::uint32_t msaaSamples);
    static bool createCppScript(std::string_view name, std::string& error);
};
