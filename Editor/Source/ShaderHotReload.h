#pragma once

#include "Platform/FileWatcher.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <future>
#include <string>
#include <utility>

namespace Editor {
    /** Rebuilds engine Slang shaders without ever blocking the render thread. */
    class ShaderHotReload final {
    public:
        ShaderHotReload(std::filesystem::path sourceRoot, std::filesystem::path buildDirectory,
                        std::filesystem::path compiledShaderDirectory,
                        std::filesystem::path runtimeShaderDirectory,
                        std::string buildConfiguration)
            : sourceRoot_(std::move(sourceRoot)), buildDirectory_(std::move(buildDirectory)),
              compiledShaderDirectory_(std::move(compiledShaderDirectory)),
              runtimeShaderDirectory_(std::move(runtimeShaderDirectory)),
              buildConfiguration_(std::move(buildConfiguration)), watcher_(sourceRoot_) {}

        ShaderHotReload(const ShaderHotReload&) = delete;
        ShaderHotReload& operator=(const ShaderHotReload&) = delete;

        void poll(const std::function<bool()>& reloadRenderer,
                  const std::function<void(const std::string&)>& info,
                  const std::function<void(const std::string&)>& warning) {
            if (build_.valid() && build_.wait_for(std::chrono::seconds::zero()) == std::future_status::ready) {
                const int result = build_.get();
                building_ = false;
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - buildStartedAt_);
                if (result != 0) {
                    warning("Shader compilation failed; keeping the active pipelines.");
                } else {
                    try {
                        std::error_code error;
                        std::filesystem::create_directories(runtimeShaderDirectory_, error);
                        std::filesystem::copy(compiledShaderDirectory_, runtimeShaderDirectory_,
                                              std::filesystem::copy_options::recursive |
                                              std::filesystem::copy_options::overwrite_existing, error);
                        if (error) throw std::filesystem::filesystem_error(
                            "Could not copy compiled shaders", compiledShaderDirectory_, runtimeShaderDirectory_, error);
                        if (reloadRenderer()) {
                            info("Shaders reloaded in " + std::to_string(elapsed.count()) + " ms.");
                        } else {
                            warning("Shader pipelines could not be recreated; restart the Editor to recover the renderer.");
                        }
                    } catch (const std::exception& exception) {
                        warning("Shader reload failed: " + std::string{exception.what()});
                    }
                }
            }

            for (const Platform::FileChange& change : watcher_.poll()) {
                if (change.path.extension() == ".slang") {
                    reloadPending_ = true;
                    lastChange_ = std::chrono::steady_clock::now();
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (!building_ && reloadPending_ && now - lastChange_ >= debounce_) {
                reloadPending_ = false;
                building_ = true;
                buildStartedAt_ = now;
                info("Shader change detected; rebuilding EngineShaders...");
                const auto buildDirectory = buildDirectory_.string();
                const auto buildConfiguration = buildConfiguration_;
                build_ = std::async(std::launch::async, [buildDirectory, buildConfiguration] {
                    const std::string command = "cmake --build \"" + buildDirectory +
                                                "\" --target EngineShaders" +
                                                (buildConfiguration.empty() ? "" : " --config " + buildConfiguration) +
                                                " --parallel";
                    return std::system(command.c_str());
                });
            }
        }

    private:
        std::filesystem::path sourceRoot_;
        std::filesystem::path buildDirectory_;
        std::filesystem::path compiledShaderDirectory_;
        std::filesystem::path runtimeShaderDirectory_;
        std::string buildConfiguration_;
        Platform::FileWatcher watcher_;
        std::future<int> build_;
        std::chrono::steady_clock::time_point lastChange_{};
        std::chrono::steady_clock::time_point buildStartedAt_{};
        static constexpr auto debounce_ = std::chrono::milliseconds{150};
        bool reloadPending_ = false;
        bool building_ = false;
    };
} // namespace Editor
