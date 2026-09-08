#include "Platform/UserPaths.h"

#include <cstdlib>

namespace Platform {

std::filesystem::path UserPaths::localData() {
    if (const char* dataHome = std::getenv("XDG_DATA_HOME"); dataHome != nullptr && *dataHome != '\0') {
        return dataHome;
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') return std::filesystem::path{home} / ".local" / "share";
    return std::filesystem::temp_directory_path();
}

std::filesystem::path UserPaths::editorData() { return localData() / "GamEngine" / "Editor"; }
std::filesystem::path UserPaths::editorLogs() { return editorData() / "Logs"; }
std::filesystem::path UserPaths::editorCrashes() { return editorData() / "Crashes"; }
std::filesystem::path UserPaths::editorState() { return editorData() / "State"; }

} // namespace Platform
