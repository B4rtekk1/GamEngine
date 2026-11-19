#pragma once

#include "Editor/Build/GameBuildPipeline.h"
#include "Engine/Plugins/PluginManager.h"
#include "Platform/RuntimePaths.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace Editor {
/**
 * Builds the project's plugins from source, like the game's C++ scripts: a
 * plugin folder in <Project>/Plugins with a Source/ folder is compiled with
 * the SDK's Plugin template whenever its library is missing or older than
 * its sources or manifest. Engine-wide plugins beside the Editor ship
 * prebuilt and are left alone.
 *
 * build() runs CMake and blocks; the Editor calls it on a worker thread and
 * reloads plugins once it finishes.
 */
class PluginBuilder final {
  public:
    struct Failure final {
        std::string plugin;
        std::string output;
    };
    struct Result final {
        std::vector<std::string> built;
        std::vector<Failure> failures;
    };

    /** Project plugins whose library is missing or out of date. */
    [[nodiscard]] static std::vector<Engine::PluginDescriptor> outdated(const std::filesystem::path& projectRoot) {
        std::vector<Engine::PluginDescriptor> plugins;
        for (const Engine::PluginStatus& status : Engine::PluginManager::discover({projectRoot / "Plugins"})) {
            if (status.state == Engine::PluginState::Failed)
                continue; // An unparsable manifest; PluginManager reports it.
            if (needsBuild(status.descriptor))
                plugins.push_back(status.descriptor);
        }
        return plugins;
    }

    [[nodiscard]] static Result build(const std::filesystem::path& editorRoot, const std::filesystem::path& projectRoot,
                                      const std::vector<Engine::PluginDescriptor>& plugins) {
        Result result;
        const auto sdk = editorRoot / "SDK";
        const auto source = sdk / "Plugin";
        if (!std::filesystem::is_regular_file(source / "CMakeLists.txt") ||
            !std::filesystem::is_regular_file(sdk / "Lib" / Platform::EngineLinkLibraryFilename)) {
            for (const auto& plugin : plugins)
                result.failures.push_back({plugin.name, "This Editor has no plugin SDK (SDK/Plugin)."});
            return result;
        }
        for (const auto& plugin : plugins) {
            const auto buildDirectory = projectRoot / "Library" / "PluginBuild" / plugin.name;
            static_cast<void>(resetBuildTreeForSource(buildDirectory, source));
            std::error_code error;
            std::filesystem::create_directories(buildDirectory, error);
            std::string output;
            const bool configured = std::filesystem::is_regular_file(buildDirectory / "CMakeCache.txt") ||
                                    run({"-S", source.string(), "-B", buildDirectory.string(),
                                         "-DGE_PLUGIN_ROOT=" + plugin.directory.string(),
                                         "-DGE_SDK_ROOT=" + sdk.string(), "-DCMAKE_BUILD_TYPE=Release"},
                                        output);
            if (configured && run({"--build", buildDirectory.string(), "--config", "Release", "--parallel"}, output))
                result.built.push_back(plugin.name);
            else
                result.failures.push_back({plugin.name, std::move(output)});
        }
        return result;
    }

  private:
    [[nodiscard]] static bool needsBuild(const Engine::PluginDescriptor& plugin) {
        const auto sources = plugin.directory / "Source";
        std::error_code error;
        if (!std::filesystem::is_directory(sources, error))
            return false; // A binary-only plugin.
        const auto library = Engine::PluginManager::findLibrary(plugin);
        if (library.empty())
            return true;
        const auto built = std::filesystem::last_write_time(library, error);
        if (error)
            return true;
        const auto newer = [&](const std::filesystem::path& path) {
            std::error_code timeError;
            const auto modified = std::filesystem::last_write_time(path, timeError);
            return !timeError && modified > built;
        };
        if (newer(plugin.manifestPath))
            return true;
        for (std::filesystem::recursive_directory_iterator it{sources, error}, end; !error && it != end;
             it.increment(error))
            if (it->is_regular_file() && newer(it->path()))
                return true;
        return false;
    }

    // Runs cmake, appending its combined output; true on exit code 0.
    [[nodiscard]] static bool run(std::vector<std::string> arguments, std::string& output) {
        arguments.insert(arguments.begin(), "cmake");
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (auto& argument : arguments)
            argv.push_back(argument.data());
        argv.push_back(nullptr);
        const SDL_PropertiesID properties = SDL_CreateProperties();
        if (!properties)
            return false;
        SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
        SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
        SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
        SDL_Process* process = SDL_CreateProcessWithProperties(properties);
        SDL_DestroyProperties(properties);
        if (!process) {
            output += "Could not start cmake: " + std::string{SDL_GetError()} + "\n";
            return false;
        }
        std::size_t size = 0;
        int exitCode = -1;
        if (void* text = SDL_ReadProcess(process, &size, &exitCode)) {
            output.append(static_cast<const char*>(text), size);
            SDL_free(text);
        }
        SDL_DestroyProcess(process);
        return exitCode == 0;
    }
};
} // namespace Editor
