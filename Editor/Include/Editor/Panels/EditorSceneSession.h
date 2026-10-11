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
    static void setProjectRoot(std::filesystem::path path, std::filesystem::path assetRoot = {});
    /** Selects the scene used by Save and Load actions. An empty path clears the selection. */
    static void setScenePath(std::filesystem::path path);
    [[nodiscard]] static std::filesystem::path scenePath();
    [[nodiscard]] static bool hasSavedScene();
    enum class FileDialogAction { SaveScene, NewScene, LoadScene, LoadProject };
    struct FileDialogResult {
        FileDialogAction action;
        std::optional<std::filesystem::path> path;
        std::string error;
        std::filesystem::path projectRoot;
    };
    /** Opens a native asynchronous dialog; consume its result on the UI thread. */
    static void requestFileDialog(FileDialogAction action);
    [[nodiscard]] static std::optional<FileDialogResult> pollFileDialog();
    /** Marks the current scene as persisted at @p path. */
    static void markSceneSaved(std::filesystem::path path);
    /** Clears the persisted-scene association after its file was removed. */
    static void clearSavedScene();
    [[nodiscard]] static Engine::AntialiasingLevel antialiasingLevel(const Engine::Renderer& renderer);

    static bool setPlayMode(bool play, Engine::ScenePreset& scene, std::string& snapshot,
                            std::string& error, Engine::AntialiasingLevel antialiasing);
    static bool createCppScript(std::string_view name, std::string& error);
};
