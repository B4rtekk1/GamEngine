#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace Platform {

/** Per-user locations owned by GamEngine rather than the executable directory. */
class UserPaths final {
public:
    [[nodiscard]] static std::filesystem::path localData();
    [[nodiscard]] static std::filesystem::path editorData();
    [[nodiscard]] static std::filesystem::path editorLogs();
    [[nodiscard]] static std::filesystem::path editorCrashes();
    [[nodiscard]] static std::filesystem::path editorState();
    [[nodiscard]] static std::filesystem::path gameData(std::string_view gameName);
    [[nodiscard]] static std::filesystem::path gameLogs(std::string_view gameName);
    [[nodiscard]] static std::filesystem::path gameCrashes(std::string_view gameName);
};

} // namespace Platform
