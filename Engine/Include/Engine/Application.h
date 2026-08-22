#pragma once

#include "Engine/Scene/Scene.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace Engine {

struct ApplicationConfig final {
    std::string title = "GamEngine Application";
    std::int32_t width = 1280;
    std::int32_t height = 720;
};

/** High-level application entry point. Owns the window and the render loop. */
class Application final {
public:
    explicit Application(ApplicationConfig config = {});
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    [[nodiscard]] Scene& scene() noexcept { return scene_; }
    [[nodiscard]] const Scene& scene() const noexcept { return scene_; }

    /** Called once per frame before rendering, when provided. */
    void setUpdateCallback(std::function<void(Scene&, float)> callback) {
        updateCallback_ = std::move(callback);
    }

    /** Starts the main loop and returns after the window is closed. */
    void run();

private:
    class Impl;
    ApplicationConfig config_;
    Scene scene_;
    std::function<void(Scene&, float)> updateCallback_;
    std::unique_ptr<Impl> impl_;
};

} // namespace Engine
