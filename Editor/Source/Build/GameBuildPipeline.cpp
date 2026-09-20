#include "Editor/Build/GameBuildPipeline.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Editor {
    namespace {
        [[nodiscard]] bool runCmake(const std::vector<std::string>& arguments, std::string& output) {
            std::vector<std::string> values{"cmake"};
            values.insert(values.end(), arguments.begin(), arguments.end());
            std::vector<char*> argv;
            argv.reserve(values.size() + 1);
            for (auto& value : values) argv.push_back(value.data());
            argv.push_back(nullptr);

            const SDL_PropertiesID properties = SDL_CreateProperties();
            if (!properties) return false;
            SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
            SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
            SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
            SDL_Process* process = SDL_CreateProcessWithProperties(properties);
            SDL_DestroyProperties(properties);
            if (!process) return false;
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
            static constexpr std::string_view extensions[]{".cpp", ".c", ".cc", ".cxx", ".h", ".hpp",
                                                            ".inl", ".vcxproj", ".sln", ".cmake"};
            const auto extension = relative.extension().string();
            return std::ranges::any_of(extensions, [&extension](const std::string_view candidate) {
                return extension == candidate;
            });
        }

        [[nodiscard]] bool excludedDirectory(const std::filesystem::path& relative) {
            for (const auto& part : relative) {
                if (part == "Scripts" || part == "ScriptBuild" || part == "ScriptModules" ||
                    part == "HotReload" || part == "ShaderSources") return true;
            }
            return false;
        }

        [[nodiscard]] std::string executableName(const std::string& name) {
            std::string result;
            result.reserve(name.size());
            for (const unsigned char character : name) {
                if (std::isalnum(character) || character == '_' || character == '-') result += static_cast<char>(character);
            }
            return result.empty() ? "Game" : result;
        }

        void copyCookedContent(const std::filesystem::path& assets, const std::filesystem::path& content) {
            std::error_code error;
            for (std::filesystem::recursive_directory_iterator iterator{assets, error}, end;
                 !error && iterator != end; iterator.increment(error)) {
                const auto relative = iterator->path().lexically_relative(assets);
                if (excludedDirectory(relative)) {
                    if (iterator->is_directory(error)) iterator.disable_recursion_pending();
                    continue;
                }
                if (iterator->is_directory(error)) continue;
                if (error || !iterator->is_regular_file(error) || isSourceOrDevelopmentFile(relative)) continue;
                const auto destination = content / relative;
                std::filesystem::create_directories(destination.parent_path(), error);
                if (!error) std::filesystem::copy_file(iterator->path(), destination,
                                                        std::filesystem::copy_options::overwrite_existing, error);
                if (error) throw std::runtime_error("Could not cook asset '" + relative.string() + "': " + error.message());
            }
            if (error) throw std::runtime_error("Could not enumerate project assets: " + error.message());
        }
    } // namespace

    GameBuildPipeline::GameBuildPipeline(std::filesystem::path editorRoot)
        : editorRoot_(std::move(editorRoot)) {}

    GameBuildResult GameBuildPipeline::build(const Engine::Project& project,
                                              const GameBuildSettings& settings) const {
        GameBuildResult result;
        try {
            const auto runtime = editorRoot_ / "Runtime" / "Win64" / "Release";
            const auto scriptsSource = editorRoot_ / "SDK" / "GameScripts";
            const auto sdk = editorRoot_ / "SDK";
            const auto output = settings.outputDirectory.empty()
                                    ? project.rootPath() / "Build" / "Windows" / executableName(project.name())
                                    : settings.outputDirectory;
            if (!std::filesystem::is_regular_file(runtime / "GamEnginePlayer.exe"))
                throw std::runtime_error("Runtime template is missing GamEnginePlayer.exe. Rebuild or reinstall the Editor.");
            if (!std::filesystem::is_regular_file(scriptsSource / "CMakeLists.txt") ||
                !std::filesystem::is_regular_file(sdk / "Lib" / "Engine.lib"))
                throw std::runtime_error("Game scripting SDK is unavailable in this Editor installation.");
            if (!std::filesystem::is_regular_file(project.startupScene()))
                throw std::runtime_error("Startup scene does not exist: " + project.startupScene().string());

            const auto scriptBuild = project.rootPath() / "Library" / "ScriptBuild";
            std::error_code error;
            std::filesystem::create_directories(scriptBuild, error);
            if (error) throw std::runtime_error("Could not create script build directory: " + error.message());
            std::string cmakeOutput;
            if (!runCmake({"-S", scriptsSource.string(), "-B", scriptBuild.string(),
                           "-DGE_PROJECT_ROOT=" + project.rootPath().string(),
                           "-DGE_SDK_ROOT=" + sdk.string()}, cmakeOutput) ||
                !runCmake({"--build", scriptBuild.string(), "--target", "GameScripts", "--config", "Release"}, cmakeOutput)) {
                throw std::runtime_error("Could not build GameScripts.dll" +
                                         (cmakeOutput.empty() ? std::string{} : ":\n" + cmakeOutput));
            }
            const auto scripts = project.rootPath() / "Library" / "ScriptModules" / "GameScripts.dll";
            if (!std::filesystem::is_regular_file(scripts)) throw std::runtime_error("GameScripts.dll was not produced by CMake.");

            if (settings.cleanBuild && std::filesystem::exists(output)) std::filesystem::remove_all(output, error);
            if (error) throw std::runtime_error("Could not clean build directory: " + error.message());
            std::filesystem::create_directories(output, error);
            if (error) throw std::runtime_error("Could not create build directory: " + error.message());
            std::filesystem::copy(runtime, output, std::filesystem::copy_options::recursive |
                                                  std::filesystem::copy_options::overwrite_existing, error);
            if (error) throw std::runtime_error("Could not copy runtime template: " + error.message());

            const std::string gameName = executableName(project.name());
            const auto executable = output / (gameName + ".exe");
            std::filesystem::rename(output / "GamEnginePlayer.exe", executable, error);
            if (error) throw std::runtime_error("Could not name game executable: " + error.message());
            std::filesystem::copy_file(scripts, output / "GameScripts.dll",
                                       std::filesystem::copy_options::overwrite_existing, error);
            if (error) throw std::runtime_error("Could not copy GameScripts.dll: " + error.message());
            copyCookedContent(project.assetRoot(), output / "Content");

            const auto startupRelative = project.startupScene().lexically_relative(project.assetRoot());
            std::ofstream manifest{output / "GamEngine.project", std::ios::trunc};
            manifest << "# Generated by GamEngine Build Game\nname = " << project.name()
                     << "\nasset_root = Content\nstartup_scene = Content/" << startupRelative.generic_string() << '\n';
            if (!manifest) throw std::runtime_error("Could not write release GamEngine.project");
            if (!std::filesystem::is_regular_file(executable) || !std::filesystem::is_regular_file(output / "Engine.dll") ||
                !std::filesystem::is_regular_file(output / "SDL3.dll") || !std::filesystem::is_regular_file(output / "GameScripts.dll") ||
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
