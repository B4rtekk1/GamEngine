/**
 * @file EditorSceneSession.cpp
 * @brief Implements editor-scene persistence, play-mode transitions and C++
 *        script generation.
 */

#include "Editor/Panels/EditorSceneSession.h"

#include "Engine/Renderer/Renderer.h"
#include "Engine/Scene/SceneSerializer.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>

namespace {
std::filesystem::path activeScenePath;
std::filesystem::path activeProjectRoot;
std::filesystem::path activeAssetRoot;
bool sceneHasBeenSaved = false;

std::filesystem::path defaultSceneDirectory() {
    return activeAssetRoot / "Scenes";
}
struct SceneDialogState {
    std::mutex mutex;
    bool ready{};
    EditorSceneSession::FileDialogResult result;
    std::string initialPath;
};
std::shared_ptr<SceneDialogState> activeDialog;

void SDLCALL sceneDialogFinished(void* userdata, const char* const* files, int) {
    const std::unique_ptr<std::shared_ptr<SceneDialogState>> owner{
        static_cast<std::shared_ptr<SceneDialogState>*>(userdata)};
    const auto& state = *owner;
    std::lock_guard lock(state->mutex);
    try {
        if (!files)
            state->result.error = SDL_GetError();
        else if (*files) {
            const std::string_view utf8{*files};
            state->result.path = std::filesystem::path{std::u8string{utf8.begin(), utf8.end()}};
            if ((state->result.action == EditorSceneSession::FileDialogAction::SaveScene ||
                 state->result.action == EditorSceneSession::FileDialogAction::NewScene) &&
                state->result.path->extension().empty())
                *state->result.path += ".scene";
        }
    } catch (const std::exception& error) {
        state->result.error = error.what();
    }
    state->ready = true;
}

} // namespace

void EditorSceneSession::setProjectRoot(std::filesystem::path path, std::filesystem::path assetRoot) {
    activeProjectRoot = std::filesystem::absolute(std::move(path)).lexically_normal();
    activeAssetRoot = assetRoot.empty() ? activeProjectRoot / "Assets" : std::move(assetRoot);
}

/**
 * @brief Returns the path of the scene used by the editor session.
 *
 * @return The active scene path, or an empty path when no scene is selected.
 */
std::filesystem::path EditorSceneSession::scenePath() {
    return activeScenePath;
}

void EditorSceneSession::setScenePath(std::filesystem::path path) {
    activeScenePath = std::move(path);
    // A project-created scene already has its canonical destination
    // (Assets/Scenes/Main.scene).  Ctrl+S must use that destination even
    // before its first overwrite, rather than forcing an unnecessary Save As.
    sceneHasBeenSaved = !activeScenePath.empty();
}

bool EditorSceneSession::hasSavedScene() {
    return sceneHasBeenSaved;
}

void EditorSceneSession::requestFileDialog(const FileDialogAction action) {
    if (activeDialog)
        return;
    static constexpr SDL_DialogFileFilter sceneFilter{"GamEngine scenes", "scene"};
    static constexpr SDL_DialogFileFilter projectFilter{"GamEngine projects", "project"};
    activeDialog = std::make_shared<SceneDialogState>();
    activeDialog->result.action = action;
    activeDialog->result.projectRoot = activeProjectRoot;
    const bool saving = action == FileDialogAction::SaveScene || action == FileDialogAction::NewScene;
    const auto initial = action == FileDialogAction::LoadProject ? activeProjectRoot
                         : saving ? (action == FileDialogAction::NewScene || activeScenePath.empty()
                                         ? defaultSceneDirectory() / "Untitled.scene"
                                         : activeScenePath)
                                  : defaultSceneDirectory();
    const auto utf8 = initial.u8string();
    activeDialog->initialPath.assign(utf8.begin(), utf8.end());
    auto* userdata = new std::shared_ptr<SceneDialogState>{activeDialog};
    const auto* filter = action == FileDialogAction::LoadProject ? &projectFilter : &sceneFilter;
    if (saving)
        SDL_ShowSaveFileDialog(sceneDialogFinished, userdata, SDL_GetKeyboardFocus(), filter, 1,
                               activeDialog->initialPath.c_str());
    else
        SDL_ShowOpenFileDialog(sceneDialogFinished, userdata, SDL_GetKeyboardFocus(), filter, 1,
                               activeDialog->initialPath.c_str(), false);
}

std::optional<EditorSceneSession::FileDialogResult> EditorSceneSession::pollFileDialog() {
    if (!activeDialog)
        return std::nullopt;
    std::optional<FileDialogResult> result;
    {
        std::lock_guard lock(activeDialog->mutex);
        if (!activeDialog->ready)
            return std::nullopt;
        result = std::move(activeDialog->result);
    }
    activeDialog.reset();
    return result;
}

void EditorSceneSession::markSceneSaved(std::filesystem::path path) {
    activeScenePath = std::move(path);
    sceneHasBeenSaved = true;
}

void EditorSceneSession::clearSavedScene() {
    activeScenePath.clear();
    sceneHasBeenSaved = false;
}

/**
 * @brief Returns the renderer's complete antialiasing selection.
 *
 * @param renderer Renderer whose antialiasing configuration is queried.
 *
 * @return The active antialiasing mode.
 */
Engine::AntialiasingLevel EditorSceneSession::antialiasingLevel(const Engine::Renderer& renderer) {
    return renderer.antialiasingLevel();
}

/**
 * @brief Enters or leaves play mode while preserving the editable scene.
 *
 * When @p play is `true`, the current scene is serialized into @p snapshot.
 * When @p play is `false`, the scene is restored from @p snapshot and the
 * snapshot string is cleared.
 *
 * @param play `true` to capture the scene before entering play mode; `false`
 *             to restore the captured scene when leaving play mode.
 * @param scene Scene that is serialized or restored.
 * @param snapshot Storage for the serialized editor-scene state.
 * @param error Receives an exception message on failure and is cleared on
 *              success.
 * @param antialiasing Antialiasing mode forwarded to the scene serializer.
 *
 * @retval true The scene was captured or restored successfully.
 * @retval false Serialization or deserialization threw an exception.
 */
// NOLINTBEGIN(readability-identifier-length)

bool EditorSceneSession::setPlayMode(const bool play, Engine::ScenePreset& scene, std::string& snapshot,
                                     std::string& error, const Engine::AntialiasingLevel antialiasing) {
    try {
        if (play) {
            std::ostringstream output;
            Engine::SceneSerializer::save(scene, output, antialiasing);
            snapshot = std::move(output).str();
        } else {
            std::istringstream input{snapshot};
            Engine::SceneSerializer::load(scene, input);
            snapshot.clear();
        }
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

/**
 * @brief Generates a C++ script class and registers it with the engine.
 *
 * The class name must begin with an alphabetic character and may otherwise
 * contain only alphanumeric characters and underscores. On success, a header
 * and source file are created in the active project's `Assets/Scripts` directory.
 *
 * @param name Name of the generated C++ class and the base name of both files.
 * @param error Receives a human-readable failure description.
 *
 * @retval true Both generated files were written successfully.
 * @retval false The class name is invalid, either output file already exists,
 *               or the files could not be created or written.
 */
bool EditorSceneSession::createCppScript(const std::string_view name, std::string& error) {
    if (name.empty() || !std::isalpha(static_cast<unsigned char>(name.front())) ||
        !std::ranges::all_of(name,
                             [](const char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; })) {
        error = "Use a valid C++ class name.";
        return false;
    }
    if (activeProjectRoot.empty()) {
        error = "No project is currently open.";
        return false;
    }
    const auto directory = activeAssetRoot / "Scripts";
    const auto header = directory / (std::string{name} + ".h");
    const auto source = directory / (std::string{name} + ".cpp");
    if (std::filesystem::exists(header) || std::filesystem::exists(source)) {
        error = "Script already exists.";
        return false;
    }
    std::filesystem::create_directories(directory);
    std::ofstream h{header}, cpp{source};
    if (!h || !cpp) {
        error = "Could not create script files.";
        return false;
    }
    h << "// Generated by GamEngine. You can safely edit this script.\n"
      << "#pragma once\n#include <Engine/Scripting/Script.h>\n#include <Engine/Scripting/ScriptAttributes.h>\n\nclass "
      << name << " final : public Engine::Script {\npublic:\n    void onCreate() override {}\n"
      << "    void onEnable() override {}\n    void onUpdate(float deltaTime) override { (void)deltaTime; }\n"
      << "    void onDisable() override {}\n    void onDestroy() override {}\n};\n";
    cpp << "// Generated by GamEngine. You can safely edit this script.\n#include \"" << name << ".h\"\n";
    return static_cast<bool>(h) && static_cast<bool>(cpp);
}

// NOLINTEND(readability-identifier-length)
