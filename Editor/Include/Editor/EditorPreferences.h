#pragma once

#include <filesystem>

namespace Editor {

struct EditorSession final {
    std::filesystem::path projectManifest;
    std::filesystem::path scenePath;
};

[[nodiscard]] std::filesystem::path preferencesDirectory();
[[nodiscard]] EditorSession loadSession();
void saveSession(const EditorSession& session);

} // namespace Editor
