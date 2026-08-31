#include "Editor/EditorPreferences.h"

#include <fstream>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Editor {

std::filesystem::path preferencesDirectory() {
#ifdef _WIN32
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (length > 1) {
        std::vector<wchar_t> appData(length);
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", appData.data(), length) != 0) {
            return std::filesystem::path{appData.data()} / "GamEngine" / "Editor";
        }
    }
#endif
    return std::filesystem::temp_directory_path() / "GamEngine" / "Editor";
}

EditorSession loadSession() {
    EditorSession session;
    std::ifstream input{preferencesDirectory() / "session.ini"};
    std::string line;
    while (std::getline(input, line)) {
        const auto delimiter = line.find('=');
        if (delimiter == std::string::npos) continue;
        const auto value = line.substr(delimiter + 1);
        if (line.starts_with("project=")) session.projectManifest = value;
        else if (line.starts_with("scene=")) session.scenePath = value;
    }
    return session;
}

void saveSession(const EditorSession& session) {
    const auto directory = preferencesDirectory();
    std::filesystem::create_directories(directory);
    std::ofstream output{directory / "session.ini", std::ios::trunc};
    if (!output) throw std::runtime_error("Could not write editor session");
    output << "project=" << session.projectManifest.string() << '\n'
           << "scene=" << session.scenePath.string() << '\n';
}

} // namespace Editor
