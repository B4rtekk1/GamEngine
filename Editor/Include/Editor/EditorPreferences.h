#pragma once

#include <filesystem>
#include <vector>

namespace Editor {

struct EditorSession final {
    std::filesystem::path projectManifest;
    std::filesystem::path scenePath;
};

[[nodiscard]] std::filesystem::path preferencesDirectory();
[[nodiscard]] EditorSession loadSession();
void saveSession(const EditorSession& session);

/** User-tunable editor settings stored next to the session file. */
struct EditorPreferences final {
    float uiScale = 1.0F;
};

[[nodiscard]] EditorPreferences loadPreferences();
void savePreferences(const EditorPreferences& preferences);

/** Most-recently-opened project manifests, newest first. */
[[nodiscard]] std::vector<std::filesystem::path> loadRecentProjects();
void addRecentProject(const std::filesystem::path& manifest);

} // namespace Editor
