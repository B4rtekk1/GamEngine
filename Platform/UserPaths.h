#pragma once

#include <filesystem>

namespace Platform {

/** Per-user locations owned by GamEngine rather than the executable directory. */
class UserPaths final {
public:
    [[nodiscard]] static std::filesystem::path localData();
    [[nodiscard]] static std::filesystem::path editorData();
    [[nodiscard]] static std::filesystem::path editorLogs();
    [[nodiscard]] static std::filesystem::path editorCrashes();
    [[nodiscard]] static std::filesystem::path editorState();
};

} // namespace Platform
