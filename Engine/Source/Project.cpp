#include "Engine/Project.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

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

Project Project::create(const std::filesystem::path& rootDirectory, std::string name) {
    const auto root = std::filesystem::absolute(rootDirectory).lexically_normal();
    if (root.empty() || (std::filesystem::exists(root) && !std::filesystem::is_empty(root))) {
        throw std::runtime_error("Project directory must be empty: " + root.string());
    }
    if (name.empty()) name = root.filename().string();
    if (name.empty()) throw std::runtime_error("Project name cannot be empty");

    std::error_code error;
    std::filesystem::create_directories(root / "Assets" / "Scenes", error);
    if (error) throw std::runtime_error("Could not create project directories: " + error.message());
    std::filesystem::create_directories(root / "Assets" / "Models", error);
    if (error) throw std::runtime_error("Could not create project directories: " + error.message());
    std::filesystem::create_directories(root / "Assets" / "Textures", error);
    if (error) throw std::runtime_error("Could not create project directories: " + error.message());
    std::filesystem::create_directories(root / "Scripts", error);
    if (error) throw std::runtime_error("Could not create project directories: " + error.message());

    const auto manifest = root / "GamEngine.project";
    std::ofstream manifestOutput{manifest};
    if (!manifestOutput) throw std::runtime_error("Could not create project manifest: " + manifest.string());
    manifestOutput << "# GamEngine project\nname = " << name
                   << "\nasset_root = Assets\nstartup_scene = Assets/Scenes/Main.scene\n";
    if (!manifestOutput) throw std::runtime_error("Could not write project manifest: " + manifest.string());
    manifestOutput.close();

    const auto scene = root / "Assets" / "Scenes" / "Main.scene";
    std::ofstream sceneOutput{scene};
    if (!sceneOutput) throw std::runtime_error("Could not create starter scene: " + scene.string());
    sceneOutput << "GAMENGINE_SCENE 12\nSETTINGS MSAA 0\nMESHES 0\nENTITIES 0\nEND_SCENE\n";
    if (!sceneOutput) throw std::runtime_error("Could not write starter scene: " + scene.string());
    sceneOutput.close();
    return load(manifest);
}

std::filesystem::path Project::resolve(const std::filesystem::path& path) const {
    return path.is_absolute() ? path : (rootPath_ / path).lexically_normal();
}

std::vector<std::filesystem::path> Project::scenes() const {
    std::vector<std::filesystem::path> result;
    const auto directory = startupScene_.parent_path();
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) return result;

    for (std::filesystem::recursive_directory_iterator iterator{directory, error}, end;
         !error && iterator != end; iterator.increment(error)) {
        if (!iterator->is_regular_file(error) || iterator->path().extension() != ".scene") continue;
        result.push_back(iterator->path().lexically_normal());
    }
    std::ranges::sort(result);
    return result;
}

} // namespace Engine
