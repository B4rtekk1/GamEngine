/**
 * @file EditorSceneSession.cpp
 * @brief Implements editor-scene persistence, play-mode transitions and C++
 *        script generation.
 */

#include "Editor/Panels/EditorSceneSession.h"

#include "Engine/Renderer/Renderer.h"
#include "Engine/Scene/SceneSerializer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#endif

namespace {
std::filesystem::path activeScenePath;
bool sceneHasBeenSaved = false;
}

/**
 * @brief Returns the path of the scene used by the editor session.
 *
 * @return Absolute or source-root-relative filesystem path to
 *         `Assets/Scenes/Editor.scene`, depending on the value of
 *         `GAMEENGINE_SOURCE_DIR`.
 */
std::filesystem::path EditorSceneSession::scenePath() {
    if (!activeScenePath.empty()) return activeScenePath;
    return std::filesystem::path{GAMEENGINE_SOURCE_DIR} / "Assets" / "Scenes" / "Editor.scene";
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

std::optional<std::filesystem::path> EditorSceneSession::chooseSaveScenePath() {
#ifdef _WIN32
    std::array<wchar_t, 32768> selectedPath{};
    const std::wstring initialFilename = scenePath().filename().wstring();
    const std::size_t copyLength = std::min(initialFilename.size(), selectedPath.size() - 1);
    std::copy_n(initialFilename.begin(), copyLength, selectedPath.begin());

    const std::filesystem::path initialDirectory = scenePath().parent_path();
    const std::wstring initialDirectoryString = initialDirectory.wstring();
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = L"GamEngine Scene (*.scene)\0*.scene\0All Files (*.*)\0*.*\0\0";
    dialog.lpstrFile = selectedPath.data();
    dialog.nMaxFile = static_cast<DWORD>(selectedPath.size());
    dialog.lpstrInitialDir = initialDirectoryString.empty() ? nullptr : initialDirectoryString.c_str();
    dialog.lpstrDefExt = L"scene";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&dialog)) return std::nullopt;
    return std::filesystem::path{selectedPath.data()};
#else
    return std::nullopt;
#endif
}

std::optional<std::filesystem::path> EditorSceneSession::chooseLoadScenePath() {
#ifdef _WIN32
    std::array<wchar_t, 32768> selectedPath{};
    const std::filesystem::path initialDirectory = scenePath().parent_path();
    const std::wstring initialDirectoryString = initialDirectory.wstring();
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = L"GamEngine Scene (*.scene)\0*.scene\0All Files (*.*)\0*.*\0\0";
    dialog.lpstrFile = selectedPath.data();
    dialog.nMaxFile = static_cast<DWORD>(selectedPath.size());
    dialog.lpstrInitialDir = initialDirectoryString.empty() ? nullptr : initialDirectoryString.c_str();
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return std::nullopt;
    return std::filesystem::path{selectedPath.data()};
#else
    return std::nullopt;
#endif
}

std::optional<std::filesystem::path> EditorSceneSession::chooseLoadProjectPath() {
#ifdef _WIN32
    std::array<wchar_t, 32768> selectedPath{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = L"GamEngine Project (*.project)\0*.project\0All Files (*.*)\0*.*\0\0";
    dialog.lpstrFile = selectedPath.data();
    dialog.nMaxFile = static_cast<DWORD>(selectedPath.size());
    dialog.lpstrDefExt = L"project";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return std::nullopt;
    return std::filesystem::path{selectedPath.data()};
#else
    return std::nullopt;
#endif
}

void EditorSceneSession::markSceneSaved(std::filesystem::path path) {
    activeScenePath = std::move(path);
    sceneHasBeenSaved = true;
}

/**
 * @brief Converts the renderer's MSAA level to a numeric sample count.
 *
 * @param renderer Renderer whose antialiasing configuration is queried.
 *
 * @return `2` for MSAA 2x, `4` for MSAA 4x, or `0` when neither supported
 *         MSAA mode is active.
 */
std::uint32_t EditorSceneSession::msaaSampleCount(const Engine::Renderer &renderer) {
    return renderer.antialiasingLevel() == Engine::AntialiasingLevel::MSAA2x
               ? 2U
               : renderer.antialiasingLevel() == Engine::AntialiasingLevel::MSAA4x
                     ? 4U
                     : 0U;
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
 * @param samples MSAA sample count forwarded to the scene serializer while
 *                capturing the scene.
 *
 * @retval true The scene was captured or restored successfully.
 * @retval false Serialization or deserialization threw an exception.
 */
// NOLINTBEGIN(readability-identifier-length)

bool EditorSceneSession::setPlayMode(const bool play, Engine::ScenePreset &scene,
                                     std::string &snapshot, std::string &error, const std::uint32_t samples) {
    try {
        if (play) {
            std::ostringstream output;
            Engine::SceneSerializer::save(scene, output, samples);
            snapshot = std::move(output).str();
        } else {
            std::istringstream input{snapshot};
            Engine::SceneSerializer::load(scene, input);
            snapshot.clear();
        }
        error.clear();
        return true;
    } catch (const std::exception &exception) {
        error = exception.what();
        return false;
    }
}

/**
 * @brief Generates a C++ script class and registers it with the engine.
 *
 * The class name must begin with an alphabetic character and may otherwise
 * contain only alphanumeric characters and underscores. On success, a header
 * and source file are created in `Sandbox/Source/Scripts`.
 *
 * @param name Name of the generated C++ class and the base name of both files.
 * @param error Receives a human-readable failure description.
 *
 * @retval true Both generated files were written successfully.
 * @retval false The class name is invalid, either output file already exists,
 *               or the files could not be created or written.
 */
bool EditorSceneSession::createCppScript(const std::string_view name, std::string &error) {
    if (name.empty() || !std::isalpha(static_cast<unsigned char>(name.front())) ||
        !std::ranges::all_of(name, [](const char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        })) {
        error = "Use a valid C++ class name.";
        return false;
    }
    const auto directory = std::filesystem::path{GAMEENGINE_SOURCE_DIR} / "Sandbox/Source/Scripts";
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
            << "#pragma once\n#include <Engine/Scripting/Script.h>\n\nclass " << name
            << " final : public Engine::Script {\npublic:\n    void onCreate() override {}\n"
            << "    void onEnable() override {}\n    void onUpdate(float deltaTime) override { (void)deltaTime; }\n"
            << "    void onDisable() override {}\n    void onDestroy() override {}\n};\n";
    cpp << "// Generated by GamEngine. You can safely edit this script.\n#include \""
            << name << ".h\"\n#include <Engine/Scripting/ScriptRegistry.h>\n\nENGINE_REGISTER_SCRIPT("
            << name << ");\n";
    return static_cast<bool>(h) && static_cast<bool>(cpp);
}

// NOLINTEND(readability-identifier-length)
