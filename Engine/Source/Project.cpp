#include "Engine/Project.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace Engine {
namespace {

[[nodiscard]] std::string trim(std::string value) {
    const auto isSpace = [](const unsigned char character) { return std::isspace(character) != 0; };
    const auto first = std::find_if_not(value.begin(), value.end(), isSpace);
    const auto last = std::find_if_not(value.rbegin(), value.rend(), isSpace).base();
    return first >= last ? std::string{} : std::string{first, last};
}

[[nodiscard]] std::filesystem::path requireRelativePath(const std::string& value,
                                                         const std::string_view key) {
    const std::filesystem::path path{value};
    if (path.empty() || path.is_absolute()) {
        throw std::runtime_error("Project key '" + std::string{key} + "' must be a relative path");
    }
    for (const auto& part : path) {
        if (part == "..") {
            throw std::runtime_error("Project key '" + std::string{key} + "' cannot leave the project directory");
        }
    }
    return path.lexically_normal();
}

} // namespace

Project Project::load(const std::filesystem::path& manifestPath) {
    const auto absoluteManifest = std::filesystem::absolute(manifestPath).lexically_normal();
    std::ifstream input{absoluteManifest};
    if (!input) {
        throw std::runtime_error("Could not open project manifest: " + absoluteManifest.string());
    }

    std::unordered_map<std::string, std::string> values;
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        line = trim(std::move(line));
        if (line.empty() || line.starts_with('#') || line.starts_with(';')) continue;
        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            throw std::runtime_error("Invalid project manifest line " + std::to_string(lineNumber));
        }
        const std::string key = trim(line.substr(0, separator));
        const std::string value = trim(line.substr(separator + 1));
        if (key.empty() || value.empty() || !values.emplace(key, value).second) {
            throw std::runtime_error("Invalid or duplicate project manifest key on line " +
                                     std::to_string(lineNumber));
        }
    }

    const auto assetRoot = values.find("asset_root");
    const auto startupScene = values.find("startup_scene");
    if (assetRoot == values.end() || startupScene == values.end()) {
        throw std::runtime_error("Project manifest requires asset_root and startup_scene");
    }

    Project project;
    project.manifestPath_ = absoluteManifest;
    project.rootPath_ = absoluteManifest.parent_path();
    project.name_ = values.contains("name") ? values.at("name") : project.rootPath_.filename().string();
    project.assetRoot_ = project.resolve(requireRelativePath(assetRoot->second, "asset_root"));
    project.startupScene_ = project.resolve(requireRelativePath(startupScene->second, "startup_scene"));
    return project;
}

Project Project::discover(const std::filesystem::path& startDirectory) {
    auto directory = std::filesystem::absolute(startDirectory).lexically_normal();
    if (std::filesystem::is_regular_file(directory)) directory = directory.parent_path();
    while (!directory.empty()) {
        const auto candidate = directory / "GamEngine.project";
        if (std::filesystem::is_regular_file(candidate)) return load(candidate);
        const auto parent = directory.parent_path();
        if (parent == directory) break;
        directory = parent;
    }
    throw std::runtime_error("Could not find GamEngine.project from " + startDirectory.string());
}

Project Project::defaults(const std::filesystem::path& rootDirectory) {
    Project project;
    project.rootPath_ = std::filesystem::absolute(rootDirectory).lexically_normal();
    project.name_ = project.rootPath_.filename().string();
    project.assetRoot_ = project.rootPath_ / "Assets";
    project.startupScene_ = project.assetRoot_ / "Scenes" / "Editor.scene";
    return project;
}

std::filesystem::path Project::resolve(const std::filesystem::path& path) const {
    return path.is_absolute() ? path : (rootPath_ / path).lexically_normal();
}

} // namespace Engine
