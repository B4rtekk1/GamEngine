#pragma once

#include "Engine/ECS/Registry.h"
#include "Engine/Scripting/ScriptModuleManager.h"
#include "Engine/Scene/Scene.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <future>
#include <string>
#include <unordered_map>
#include <utility>

namespace Editor {
    class ScriptHotReload final {
    public:
        ScriptHotReload(std::filesystem::path scriptsRoot, std::filesystem::path candidatePath,
                        std::filesystem::path buildDirectory)
            : scriptsRoot_(std::move(scriptsRoot)), candidatePath_(std::move(candidatePath)),
              buildDirectory_(std::move(buildDirectory)), snapshot_(snapshot()) {}

        ScriptHotReload(const ScriptHotReload &) = delete;
        ScriptHotReload &operator=(const ScriptHotReload &) = delete;

        void poll(Engine::ScriptModuleManager &modules, Engine::Scene &scene,
                  const std::function<void(const std::string &)> &info,
                  const std::function<void(const std::string &)> &warning) {
            if (build_.valid() && build_.wait_for(std::chrono::seconds::zero()) == std::future_status::ready) {
                const int result = build_.get();
                building_ = false;
                if (result == 0) {
                    std::error_code error;
                    const auto writeTime = std::filesystem::last_write_time(candidatePath_, error);
                    if (!error && writeTime != lastLoadedModuleTime_) {
                        if (modules.tryReload(candidatePath_, scene)) {
                            lastLoadedModuleTime_ = writeTime;
                            info("Game scripts reloaded.");
                        } else {
                            warning("GameScripts.dll was built, but validation failed; keeping the active module.");
                        }
                    }
                } else {
                    warning("Game scripts build failed; keeping the active module.");
                }
            }

            const auto current = snapshot();
            if (!building_ && current != snapshot_) {
                snapshot_ = current;
                building_ = true;
                info("Game script change detected; rebuilding GameScripts.dll...");
                const auto buildDirectory = buildDirectory_.string();
                build_ = std::async(std::launch::async, [buildDirectory] {
                    return std::system(("cmake --build \"" + buildDirectory + "\" --target GameScripts").c_str());
                });
            }
        }

    private:
        using Snapshot = std::unordered_map<std::string, std::filesystem::file_time_type>;

        Snapshot snapshot() const {
            Snapshot result;
            std::error_code error;
            if (!std::filesystem::is_directory(scriptsRoot_, error)) return result;
            for (const auto &entry : std::filesystem::recursive_directory_iterator(scriptsRoot_, error)) {
                if (error) break;
                if (!entry.is_regular_file(error)) continue;
                const auto extension = entry.path().extension().string();
                if (extension == ".cpp" || extension == ".h") {
                    result.emplace(entry.path().string(), entry.last_write_time(error));
                }
            }
            return result;
        }

        std::filesystem::path scriptsRoot_;
        std::filesystem::path candidatePath_;
        std::filesystem::path buildDirectory_;
        Snapshot snapshot_;
        std::filesystem::file_time_type lastLoadedModuleTime_{};
        std::future<int> build_;
        bool building_ = false;
    };
} // namespace Editor
