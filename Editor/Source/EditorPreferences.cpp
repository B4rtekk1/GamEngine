#include "Editor/EditorPreferences.h"
#include "Platform/UserPaths.h"

#include <fstream>
#include <stdexcept>

namespace Editor {

std::filesystem::path preferencesDirectory() {
    return Platform::UserPaths::editorData();
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
