#include "Editor/EditorPreferences.h"

#include "Platform/UserPaths.h"

#include <algorithm>
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
        if (delimiter == std::string::npos)
            continue;
        const auto value = line.substr(delimiter + 1);
        if (line.starts_with("project="))
            session.projectManifest = value;
        else if (line.starts_with("scene="))
            session.scenePath = value;
    }
    return session;
}

void saveSession(const EditorSession& session) {
    const auto directory = preferencesDirectory();
    std::filesystem::create_directories(directory);
    std::ofstream output{directory / "session.ini", std::ios::trunc};
    if (!output)
        throw std::runtime_error("Could not write editor session");
    output << "project=" << session.projectManifest.string() << '\n' << "scene=" << session.scenePath.string() << '\n';
}

EditorPreferences loadPreferences() {
    EditorPreferences preferences;
    std::ifstream input{preferencesDirectory() / "preferences.ini"};
    std::string line;
    while (std::getline(input, line)) {
        if (line.starts_with("ui_scale=")) {
            try {
                preferences.uiScale = std::clamp(std::stof(line.substr(9)), 0.75F, 1.75F);
            } catch (const std::exception&) {
            }
        }
    }
    return preferences;
}

void savePreferences(const EditorPreferences& preferences) {
    std::filesystem::create_directories(preferencesDirectory());
    std::ofstream output{preferencesDirectory() / "preferences.ini", std::ios::trunc};
    output << "ui_scale=" << preferences.uiScale << '\n';
}

std::vector<std::filesystem::path> loadRecentProjects() {
    std::vector<std::filesystem::path> result;
    std::ifstream input{preferencesDirectory() / "recent_projects.txt"};
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && std::filesystem::is_regular_file(line))
            result.emplace_back(line);
    }
    return result;
}

void addRecentProject(const std::filesystem::path& manifest) {
    if (manifest.empty())
        return;
    constexpr std::size_t maxEntries = 8;
    auto entries = loadRecentProjects();
    std::erase(entries, manifest);
    entries.insert(entries.begin(), manifest);
    if (entries.size() > maxEntries)
        entries.resize(maxEntries);
    std::filesystem::create_directories(preferencesDirectory());
    std::ofstream output{preferencesDirectory() / "recent_projects.txt", std::ios::trunc};
    for (const auto& entry : entries)
        output << entry.string() << '\n';
}

} // namespace Editor
