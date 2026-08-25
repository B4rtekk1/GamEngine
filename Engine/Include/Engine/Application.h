#pragma once

#include "Engine/Scene/Scene.h"
#include "Engine/Renderer/RenderConfig.h"
#include "Engine/Assets/Content.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace Engine {

/** High-level lifecycle owned by an Application. */
class Game {
public:
    virtual ~Game() = default;

    virtual void onStart(Scene&) {}
    virtual void onUpdate(Scene&, float) {}
    virtual void onFixedUpdate(Scene&, float) {}
    virtual void onShutdown(Scene&) {}
};

struct ApplicationConfig final {
    std::string title = "GamEngine Application";
    std::int32_t width = 1280;
    std::int32_t height = 720;
    bool closeOnEscape = true;
    float fixedDeltaTime = 1.0f / 60.0f;
    std::filesystem::path assetRoot{};
    RenderConfig render{};
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
    [[nodiscard]] Assets::Content& content() noexcept { return content_; }
    [[nodiscard]] const Assets::Content& content() const noexcept { return content_; }

    /** Called once per frame before rendering, when provided. */
    void setUpdateCallback(std::function<void(Scene&, float)> callback) {
        updateCallback_ = std::move(callback);
    }

    /** Attaches a non-owning high-level game lifecycle to the application. */
    void setGame(Game& game) noexcept { game_ = &game; }

    /** Requests that the main loop exits after the current frame. */
    void stop() const noexcept;
    [[nodiscard]] bool isRunning() const noexcept;

    /** Starts the main loop and returns after the window is closed. */
    void run();

private:
    void initializeRuntime();
    void processEvents() const;
    void updateFrame(float deltaTime);
    void updatePhysics(float deltaTime);

    class Impl;
    ApplicationConfig config_;
    Scene scene_;
    Assets::Content content_;
    std::function<void(Scene&, float)> updateCallback_;
    Game* game_{};
    std::unique_ptr<Impl> impl_;
};

} // namespace Engine
