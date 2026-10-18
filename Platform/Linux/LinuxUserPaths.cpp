#include "Platform/UserPaths.h"

#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace Platform {

std::filesystem::path UserPaths::localData() {
    if (const char* dataHome = std::getenv("XDG_DATA_HOME"); dataHome != nullptr && *dataHome != '\0') {
        if (std::filesystem::path{dataHome}.is_absolute())
            return dataHome;
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
        return std::filesystem::path{home} / ".local" / "share";
    return std::filesystem::temp_directory_path();
}
std::filesystem::path UserPaths::documents() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0')
        throw std::runtime_error("Could not determine home directory");
    const std::filesystem::path homePath{home};
    auto config = homePath / ".config";
    if (const char* configHome = std::getenv("XDG_CONFIG_HOME");
        configHome && *configHome && std::filesystem::path{configHome}.is_absolute())
        config = configHome;
    // Read the freedesktop user directory configuration without executing it.
    // Desktop environments often localize Documents or put it on another disk.
    std::ifstream directories{config / "user-dirs.dirs"};
    for (std::string line; std::getline(directories, line);) {
        const auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line.compare(start, 17, "XDG_DOCUMENTS_DIR") != 0)
            continue;
        const auto equals = line.find_first_not_of(" \t", start + 17);
        if (equals == std::string::npos || line[equals] != '=')
            continue;
        const auto quote = line.find_first_not_of(" \t", equals + 1);
        if (quote == std::string::npos || line[quote] != '"')
            continue;
        std::string value;
        bool closed = false;
        for (auto index = quote + 1; index < line.size(); ++index) {
            if (line[index] == '"') {
                closed = true;
                break;
            }
            if (line[index] == '\\' && index + 1 < line.size())
                ++index;
            value += line[index];
        }
        if (!closed)
            continue;
        if (value == "$HOME")
            value = homePath.string();
        else if (value.starts_with("$HOME/"))
            value = homePath.string() + value.substr(5);
        if (std::filesystem::path{value}.is_absolute())
            return value;
    }
    return homePath / "Documents";
}

std::filesystem::path UserPaths::editorData() {
    return localData() / "GamEngine" / "Editor";
}
std::filesystem::path UserPaths::editorLogs() {
    return editorData() / "Logs";
}
std::filesystem::path UserPaths::editorCrashes() {
    return editorData() / "Crashes";
}
std::filesystem::path UserPaths::editorState() {
    return editorData() / "State";
}
std::filesystem::path UserPaths::gameData(const std::string_view gameName) {
    return localData() / "GamEngine" / "Games" / std::filesystem::path{std::string{gameName}};
}
std::filesystem::path UserPaths::gameLogs(const std::string_view gameName) {
    return gameData(gameName) / "Logs";
}
std::filesystem::path UserPaths::gameCrashes(const std::string_view gameName) {
    return gameData(gameName) / "Crashes";
}

} // namespace Platform
