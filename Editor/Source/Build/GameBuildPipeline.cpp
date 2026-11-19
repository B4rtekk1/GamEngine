#include "Editor/Build/GameBuildPipeline.h"

#include "Engine/Plugins/PluginManager.h"
#include "Platform/RuntimePaths.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Editor {
namespace {
[[nodiscard]] bool runCmake(const std::vector<std::string>& arguments, std::string& output) {
    std::vector<std::string> values{"cmake"};
    values.insert(values.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(values.size() + 1);
    for (auto& value : values)
        argv.push_back(value.data());
    argv.push_back(nullptr);

    const SDL_PropertiesID properties = SDL_CreateProperties();
    if (!properties)
        return false;
    SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_Process* process = SDL_CreateProcessWithProperties(properties);
    SDL_DestroyProperties(properties);
    if (!process)
        return false;
    size_t outputSize{};
    int exitCode = -1;
    void* captured = SDL_ReadProcess(process, &outputSize, &exitCode);
    if (captured != nullptr) {
        output.assign(static_cast<const char*>(captured), outputSize);
        SDL_free(captured);
    }
    SDL_DestroyProcess(process);
    return exitCode == 0;
}

[[nodiscard]] bool isSourceOrDevelopmentFile(const std::filesystem::path& relative) {
    static constexpr std::string_view extensions[]{".cpp", ".c",   ".cc",      ".cxx", ".h",
                                                   ".hpp", ".inl", ".vcxproj", ".sln", ".cmake"};
    const auto extension = relative.extension().string();
    return std::ranges::any_of(extensions,
                               [&extension](const std::string_view candidate) { return extension == candidate; });
}

[[nodiscard]] bool excludedDirectory(const std::filesystem::path& relative) {
    for (const auto& part : relative) {
        if (part == "Scripts" || part == "ScriptBuild" || part == "ScriptModules" || part == "HotReload" ||
            part == "ShaderSources")
            return true;
    }
    return false;
}

[[nodiscard]] std::string executableName(const std::string& name) {
    std::string result;
    result.reserve(name.size());
    for (const unsigned char character : name) {
        if (std::isalnum(character) || character == '_' || character == '-')
            result += static_cast<char>(character);
    }
    return result.empty() ? "Game" : result;
}

// Ships the plugins the project enabled (engine-wide ones beside the Editor
// and the project's own): manifest, this platform's binaries and Content/.
// Sources and build trees stay behind. The Player loads <game>/Plugins.
void copyEnabledPlugins(const std::filesystem::path& editorRoot, const Engine::Project& project,
                        const std::filesystem::path& output, std::vector<std::string>& warnings) {
    const auto statuses = Engine::PluginManager::discover({editorRoot / "Plugins", project.rootPath() / "Plugins"});
    for (const Engine::PluginStatus& status : statuses) {
        const Engine::PluginDescriptor& plugin = status.descriptor;
        if (status.state == Engine::PluginState::Failed) {
            warnings.push_back("Plugin " + plugin.name + " skipped: " + status.error);
            continue;
        }
        if (!project.pluginEnabled(plugin.name))
            continue;
        if (Engine::PluginManager::findLibrary(plugin).empty()) {
            warnings.push_back("Plugin " + plugin.name + " skipped: it has no library for " +
                               Engine::PluginManager::platformDirectory());
            continue;
        }
        const auto destination = output / "Plugins" / plugin.directory.filename();
        std::error_code error;
        std::filesystem::create_directories(destination, error);
        std::filesystem::copy_file(plugin.manifestPath, destination / plugin.manifestPath.filename(),
                                   std::filesystem::copy_options::overwrite_existing, error);
        const auto binaries = std::filesystem::path{"Binaries"} / Engine::PluginManager::platformDirectory();
        for (const auto& folder : {binaries, std::filesystem::path{"Content"}}) {
            if (!error && std::filesystem::is_directory(plugin.directory / folder)) {
                std::filesystem::create_directories(destination / folder, error);
                std::filesystem::copy(plugin.directory / folder, destination / folder,
                                      std::filesystem::copy_options::recursive |
                                          std::filesystem::copy_options::overwrite_existing,
                                      error);
            }
        }
        // A library directly in the plugin folder (no Binaries/) is valid too.
        const auto library = Engine::PluginManager::findLibrary(plugin);
        if (!error && library.parent_path() == plugin.directory)
            std::filesystem::copy_file(library, destination / library.filename(),
                                       std::filesystem::copy_options::overwrite_existing, error);
        if (error)
            throw std::runtime_error("Could not copy plugin " + plugin.name + ": " + error.message());
    }
}

[[nodiscard]] std::string lowerExtension(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return extension;
}

// Source formats the cooker turns into .gtex / .gmesh next to them.
[[nodiscard]] bool isCookableTextureSource(const std::filesystem::path& path) {
    static constexpr std::string_view extensions[]{".png", ".jpg", ".jpeg", ".tga", ".bmp", ".dds"};
    return std::ranges::find(extensions, lowerExtension(path)) != std::end(extensions);
}
[[nodiscard]] bool isCookableModelSource(const std::filesystem::path& path) {
    static constexpr std::string_view extensions[]{".fbx", ".glb", ".gltf", ".obj"};
    return std::ranges::find(extensions, lowerExtension(path)) != std::end(extensions);
}

// Files whose text names other assets: scenes and their sidecars, materials,
// Shader Graphs. A source asset named in one of them is loaded by path.
[[nodiscard]] bool namesOtherAssets(const std::filesystem::path& path) {
    static constexpr std::string_view extensions[]{".scene", ".terrain",  ".environment", ".gi",
                                                   ".post",  ".surface",  ".fog",         ".shadergraph",
                                                   ".mat",   ".material", ".json",        ".prefab"};
    return std::ranges::find(extensions, lowerExtension(path)) != std::end(extensions);
}

// Rewrites "MESH_ASSET <id> "<absolute path in assets>"" to the path relative
// to the asset root. Scenes saved before relative mesh paths existed name the
// author's own disk; the exported game resolves relative paths against its
// Content/ folder (SceneSerializer).
[[nodiscard]] std::string portableScene(const std::string& text, const std::filesystem::path& assets) {
    std::istringstream input{text};
    std::string result;
    result.reserve(text.size());
    std::string line;
    while (std::getline(input, line)) {
        if (line.starts_with("MESH_ASSET ")) {
            std::istringstream fields{line};
            std::string record;
            std::size_t id = 0;
            std::string path;
            if (fields >> record >> id >> std::quoted(path)) {
                const auto absolute = std::filesystem::path{path}.lexically_normal();
                const auto relative = absolute.lexically_relative(assets.lexically_normal());
                if (absolute.is_absolute() && !relative.empty() && *relative.begin() != "..") {
                    std::ostringstream rewritten;
                    rewritten << "MESH_ASSET " << id << ' ' << std::quoted(relative.generic_string());
                    line = rewritten.str();
                }
            }
        }
        result += line;
        result += '\n';
    }
    return result;
}

// Every quoted string in @p text that names a file under @p assets, as a
// path relative to it. Scenes and their sidecars quote all asset paths.
[[nodiscard]] std::vector<std::filesystem::path> quotedAssetPaths(const std::string& text,
                                                                 const std::filesystem::path& assets) {
    std::vector<std::filesystem::path> paths;
    for (std::size_t open = text.find('"'); open != std::string::npos;) {
        const std::size_t close = text.find('"', open + 1);
        if (close == std::string::npos)
            break;
        std::filesystem::path candidate{text.substr(open + 1, close - open - 1)};
        open = text.find('"', close + 1);
        if (candidate.empty())
            continue;
        if (candidate.is_absolute())
            candidate = candidate.lexically_normal().lexically_relative(assets.lexically_normal());
        candidate = candidate.lexically_normal();
        std::error_code error;
        if (!candidate.empty() && *candidate.begin() != ".." && !candidate.is_absolute() &&
            std::filesystem::is_regular_file(assets / candidate, error))
            paths.push_back(candidate);
    }
    return paths;
}

// The assets a game starting in @p startupScene needs (startupSceneOnly).
[[nodiscard]] std::unordered_set<std::string> startupSceneAssets(const std::filesystem::path& assets,
                                                                 const std::filesystem::path& startupScene,
                                                                 const std::vector<std::filesystem::path>& files) {
    std::unordered_set<std::string> needed;
    std::unordered_set<std::string> packageFolders;
    const auto scene = startupScene.lexically_normal().lexically_relative(assets.lexically_normal());
    const std::string sceneName = scene.generic_string();
    for (const auto& relative : files) {
        const std::string name = relative.generic_string();
        // The scene and its sidecars (Main.scene.environment, ...).
        if (name != sceneName && !name.starts_with(sceneName + "."))
            continue;
        needed.insert(name);
        std::ifstream input{assets / relative, std::ios::binary};
        const std::string text{std::istreambuf_iterator<char>{input}, {}};
        for (const auto& referenced : quotedAssetPaths(text, assets)) {
            needed.insert(referenced.generic_string());
            // A mesh's cooked textures sit in its asset folder.
            if (lowerExtension(referenced) == ".gmesh" && std::distance(referenced.begin(), referenced.end()) > 1)
                packageFolders.insert(referenced.begin()->generic_string());
            // A texture named by path loads its cooked sibling when current.
            needed.insert((referenced.parent_path() / referenced.stem()).generic_string() + ".gtex");
        }
    }
    for (const auto& relative : files)
        if (relative.begin() != relative.end() && packageFolders.contains(relative.begin()->generic_string()))
            needed.insert(relative.generic_string());
    return needed;
}

void copyCookedContent(const std::filesystem::path& assets, const std::filesystem::path& content,
                       const std::filesystem::path* startupSceneOnly = nullptr) {
    std::error_code error;
    std::vector<std::filesystem::path> files;
    for (std::filesystem::recursive_directory_iterator iterator{assets, error}, end; !error && iterator != end;
         iterator.increment(error)) {
        const auto relative = iterator->path().lexically_relative(assets);
        if (excludedDirectory(relative)) {
            if (iterator->is_directory(error))
                iterator.disable_recursion_pending();
            continue;
        }
        if (iterator->is_directory(error))
            continue;
        if (error || !iterator->is_regular_file(error) || isSourceOrDevelopmentFile(relative))
            continue;
        files.push_back(relative);
    }
    if (error)
        throw std::runtime_error("Could not enumerate project assets: " + error.message());
    if (startupSceneOnly != nullptr) {
        const auto needed = startupSceneAssets(assets, *startupSceneOnly, files);
        std::erase_if(files, [&](const std::filesystem::path& relative) {
            return !needed.contains(relative.generic_string());
        });
    }

    // Cooked artifacts by folder and base name ("Wood.bc5.gtex" covers "Wood"),
    // and the text of every file that names assets by path.
    std::unordered_set<std::string> cookedTextures;
    std::unordered_set<std::string> cookedModels;
    std::string references;
    for (const auto& relative : files) {
        const std::string extension = lowerExtension(relative);
        if (extension == ".gtex") {
            const auto withoutGtex = relative.parent_path() / relative.stem();
            cookedTextures.insert(withoutGtex.generic_string());
            cookedTextures.insert((withoutGtex.parent_path() / withoutGtex.stem()).generic_string());
        } else if (extension == ".gmesh") {
            cookedModels.insert((relative.parent_path() / relative.stem()).generic_string());
        } else if (namesOtherAssets(relative)) {
            std::ifstream input{assets / relative, std::ios::binary};
            references.append(std::istreambuf_iterator<char>{input}, {});
            references += '\n';
        }
    }
    // A cooked source the game never loads by its own path stays behind: the
    // .gmesh points at its .gtex textures directly. A source named by a scene,
    // material or graph is kept, since loading it needs the source itself.
    const auto shippedOnlyCooked = [&](const std::filesystem::path& relative) {
        const std::string key = (relative.parent_path() / relative.stem()).generic_string();
        const bool cooked = (isCookableTextureSource(relative) && cookedTextures.contains(key)) ||
                            (isCookableModelSource(relative) && cookedModels.contains(key));
        return cooked && references.find(relative.filename().string()) == std::string::npos;
    };

    for (const auto& relative : files) {
        if (shippedOnlyCooked(relative))
            continue;
        const auto destination = content / relative;
        std::filesystem::create_directories(destination.parent_path(), error);
        if (!error && lowerExtension(relative) == ".scene") {
            std::ifstream input{assets / relative, std::ios::binary};
            const std::string text{std::istreambuf_iterator<char>{input}, {}};
            std::ofstream output{destination, std::ios::binary | std::ios::trunc};
            output << portableScene(text, assets);
            if (!input || !output)
                error = std::make_error_code(std::errc::io_error);
        } else if (!error) {
            std::filesystem::copy_file(assets / relative, destination,
                                       std::filesystem::copy_options::overwrite_existing, error);
        }
        if (error)
            throw std::runtime_error("Could not cook asset '" + relative.string() + "': " + error.message());
    }
}
} // namespace

bool resetBuildTreeForSource(const std::filesystem::path& buildDirectory, const std::filesystem::path& sourceDirectory) {
    std::ifstream cache{buildDirectory / "CMakeCache.txt"};
    if (!cache)
        return true;
    constexpr std::string_view Key = "CMAKE_HOME_DIRECTORY:INTERNAL=";
    std::string line;
    while (std::getline(cache, line)) {
        if (!line.starts_with(Key))
            continue;
        std::error_code error;
        const auto cached = std::filesystem::weakly_canonical(std::filesystem::path{line.substr(Key.size())}, error);
        const auto current = std::filesystem::weakly_canonical(sourceDirectory, error);
        if (cached == current)
            return true;
        cache.close();
        std::filesystem::remove_all(buildDirectory, error);
        return !error;
    }
    return true;
}

GameBuildPipeline::GameBuildPipeline(std::filesystem::path editorRoot) : editorRoot_(std::move(editorRoot)) {}

GameBuildResult GameBuildPipeline::build(const Engine::Project& project, const GameBuildSettings& settings) const {
    GameBuildResult result;
    try {
        const auto runtime = editorRoot_ / Platform::RuntimeDirectory;
        const auto scriptsSource = editorRoot_ / "SDK" / "GameScripts";
        const auto sdk = editorRoot_ / "SDK";
        const auto output = settings.outputDirectory.empty() ? project.rootPath() / "Build" / Platform::BuildPlatform /
                                                                   executableName(project.name())
                                                             : settings.outputDirectory;
        if (!std::filesystem::is_regular_file(runtime / Platform::PlayerFilename))
            throw std::runtime_error(
                "Runtime template is missing the Player executable. Rebuild or reinstall the Editor.");
        if (!std::filesystem::is_regular_file(scriptsSource / "CMakeLists.txt") ||
            !std::filesystem::is_regular_file(sdk / "Lib" / Platform::EngineLinkLibraryFilename))
            throw std::runtime_error("Game scripting SDK is unavailable in this Editor installation.");
        if (!std::filesystem::is_regular_file(project.startupScene()))
            throw std::runtime_error("Startup scene does not exist: " + project.startupScene().string());

        const auto scriptBuild = project.rootPath() / "Library" / "ScriptBuild";
        if (!resetBuildTreeForSource(scriptBuild, scriptsSource))
            throw std::runtime_error("Could not reset the script build directory made by another Editor.");
        std::error_code error;
        std::filesystem::create_directories(scriptBuild, error);
        if (error)
            throw std::runtime_error("Could not create script build directory: " + error.message());
        std::string cmakeOutput;
        if (!runCmake({"-S", scriptsSource.string(), "-B", scriptBuild.string(),
                       "-DGE_PROJECT_ROOT=" + project.rootPath().string(),
                       "-DGE_ASSET_ROOT=" + project.assetRoot().string(), "-DGE_SDK_ROOT=" + sdk.string(),
                       "-DCMAKE_BUILD_TYPE=Release"},
                      cmakeOutput) ||
            !runCmake({"--build", scriptBuild.string(), "--target", "GameScripts", "--config", "Release"},
                      cmakeOutput)) {
            throw std::runtime_error("Could not build game script module" +
                                     (cmakeOutput.empty() ? std::string{} : ":\n" + cmakeOutput));
        }
        const auto scripts = project.rootPath() / "Library" / "ScriptModules" / Platform::ScriptModuleFilename;
        if (!std::filesystem::is_regular_file(scripts))
            throw std::runtime_error("Game script module was not produced by CMake.");

        if (settings.cleanBuild && std::filesystem::exists(output))
            std::filesystem::remove_all(output, error);
        if (error)
            throw std::runtime_error("Could not clean build directory: " + error.message());
        std::filesystem::create_directories(output, error);
        if (error)
            throw std::runtime_error("Could not create build directory: " + error.message());
        std::filesystem::copy(
            runtime, output,
            std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, error);
        if (error)
            throw std::runtime_error("Could not copy runtime template: " + error.message());

        const std::string gameName = executableName(project.name());
        const auto executable = output / (gameName + Platform::ExecutableSuffix);
        std::filesystem::rename(output / Platform::PlayerFilename, executable, error);
        if (error)
            throw std::runtime_error("Could not name game executable: " + error.message());
        std::filesystem::copy_file(scripts, output / Platform::ScriptModuleFilename,
                                   std::filesystem::copy_options::overwrite_existing, error);
        if (error)
            throw std::runtime_error("Could not copy game script module: " + error.message());
        copyCookedContent(project.assetRoot(), output / "Content",
                          settings.startupSceneOnly ? &project.startupScene() : nullptr);
        copyEnabledPlugins(editorRoot_, project, output, result.warnings);

        const auto startupRelative = project.startupScene().lexically_relative(project.assetRoot());
        std::ofstream manifest{output / "GamEngine.project", std::ios::trunc};
        manifest << "# Generated by GamEngine Build Game\nname = " << project.name()
                 << "\nasset_root = Content\nstartup_scene = Content/" << startupRelative.generic_string() << '\n';
        if (!manifest)
            throw std::runtime_error("Could not write release GamEngine.project");
        if (!std::filesystem::is_regular_file(executable) ||
            !std::filesystem::is_regular_file(output / Platform::EngineLibraryFilename) ||
            !std::filesystem::is_regular_file(output / "Assets" / "BRDF" / "brdf_lut.bin") ||
            !std::filesystem::is_regular_file(output / Platform::ScriptModuleFilename) ||
            !std::filesystem::is_regular_file(output / "Content" / startupRelative))
            throw std::runtime_error("Final game build validation failed.");
        result.success = true;
        result.executable = executable;
    } catch (const std::exception& exception) {
        result.errors.emplace_back(exception.what());
    }
    return result;
}
} // namespace Editor
