#pragma once

#include "Engine/ECS/Registry.h"
#include "Engine/Scripting/ScriptModuleManager.h"
#include "Engine/Scene/Scene.h"
#include "Platform/FileWatcher.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace Editor {
    class ScriptHotReload final {
    public:
        ScriptHotReload(std::filesystem::path scriptsRoot, std::filesystem::path candidatePath,
                        std::filesystem::path buildDirectory, std::string buildConfiguration)
            : scriptsRoot_(std::move(scriptsRoot)), candidatePath_(std::move(candidatePath)),
              buildDirectory_(std::move(buildDirectory)), buildConfiguration_(std::move(buildConfiguration)),
              watcher_(scriptsRoot_) {}

        ScriptHotReload(const ScriptHotReload &) = delete;
        ScriptHotReload &operator=(const ScriptHotReload &) = delete;

        void setProject(std::filesystem::path scriptsRoot) {
            if (scriptsRoot_ == scriptsRoot) return;
            scriptsRoot_ = std::move(scriptsRoot);
            watcher_.watch(scriptsRoot_);
            reloadPending_ = false;
        }

        void requestBuild() {
            reloadPending_ = true;
            lastChange_ = std::chrono::steady_clock::now() - debounce_;
        }

        void poll(Engine::ScriptModuleManager &modules, Engine::Scene &scene,
                  const std::function<void(const std::string &)> &info,
                  const std::function<void(const std::string &)> &warning) {
            if (build_.valid() && build_.wait_for(std::chrono::seconds::zero()) == std::future_status::ready) {
                const CmakeResult result = build_.get();
                building_ = false;
                const auto buildDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - buildStartedAt_);
                if (result.exitCode == 0) {
                    info("Game scripts build completed in " + std::to_string(buildDuration.count()) + " ms.");
                    std::error_code error;
                    const auto writeTime = std::filesystem::last_write_time(candidatePath_, error);
                    if (!error && writeTime != lastLoadedModuleTime_) {
                        const auto reloadStartedAt = std::chrono::steady_clock::now();
                        if (modules.tryReload(candidatePath_, scene)) {
                            lastLoadedModuleTime_ = writeTime;
                            const auto reloadDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - reloadStartedAt);
                            info("Game scripts reloaded in " + std::to_string(reloadDuration.count()) + " ms.");
                        } else {
                            warning("GameScripts.dll was built, but validation failed; keeping the active module.");
                        }
                    }
                } else {
                    warning("Game scripts build failed; keeping the active module." +
                            (result.output.empty() ? "" : "\n" + result.output));
                }
            }

            // A save commonly emits several native events.  Coalesce them so
            // an IDE's atomic-save sequence produces exactly one build.
            const auto changes = watcher_.poll();
            if (!changes.empty()) {
                reloadPending_ = true;
                lastChange_ = std::chrono::steady_clock::now();
            }
            const auto now = std::chrono::steady_clock::now();
            if (!building_ && reloadPending_ && now - lastChange_ >= debounce_) {
                reloadPending_ = false;
                building_ = true;
                buildStartedAt_ = now;
                info("Game script change detected; rebuilding GameScripts.dll...");
                const auto buildDirectory = buildDirectory_.string();
                const auto buildConfiguration = buildConfiguration_;
                build_ = std::async(std::launch::async, [buildDirectory, buildConfiguration] {
                    return runCmake({"--build", buildDirectory, "--target", "GameScripts",
                                     "--parallel"}, buildConfiguration);
                });
            }
        }

    private:
        struct CmakeResult {
            int exitCode = -1;
            std::string output;
        };

        static CmakeResult runCmake(std::initializer_list<std::string> arguments,
                                    const std::string &configuration = {}) {
            std::vector<std::string> values{"cmake"};
            values.insert(values.end(), arguments.begin(), arguments.end());
            if (!configuration.empty()) {
                values.emplace_back("--config");
                values.push_back(configuration);
            }
            std::vector<char *> argv;
            argv.reserve(values.size() + 1);
            for (const auto &value : values) argv.push_back(const_cast<char *>(value.c_str()));
            argv.push_back(nullptr);

            const SDL_PropertiesID properties = SDL_CreateProperties();
            if (!properties) return {};
            SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
            SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
            SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
            SDL_Process *process = SDL_CreateProcessWithProperties(properties);
            SDL_DestroyProperties(properties);
            if (!process) return {};

            size_t outputSize = 0;
            int exitCode = -1;
            void *output = SDL_ReadProcess(process, &outputSize, &exitCode);
            CmakeResult result{.exitCode = exitCode};
            if (output) {
                result.output.assign(static_cast<const char *>(output), outputSize);
                SDL_free(output);
            }
            SDL_DestroyProcess(process);
            return result;
        }

        std::filesystem::path scriptsRoot_;
        std::filesystem::path candidatePath_;
        std::filesystem::path buildDirectory_;
        std::string buildConfiguration_;
        Platform::FileWatcher watcher_;
        std::filesystem::file_time_type lastLoadedModuleTime_{};
        std::future<CmakeResult> build_;
        std::chrono::steady_clock::time_point lastChange_{};
        std::chrono::steady_clock::time_point buildStartedAt_{};
        static constexpr auto debounce_ = std::chrono::milliseconds{150};
        bool reloadPending_ = false;
        bool building_ = false;
    };
} // namespace Editor
