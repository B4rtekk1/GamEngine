#include "Engine/Application.h"

#include "Engine/Core/Time.h"
#include "Engine/Core/Profiler.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Scripting/ScriptSystem.h"
#include "Engine/Physics/PhysicsSystem.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Engine {
    class Application::Impl final {
    public:
        class SdlRuntime final {
        public:
            void initialize() {
                if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
                    throw std::runtime_error(SDL_GetError());
                }
                initialized_ = true;
            }

            ~SdlRuntime() {
                if (initialized_) SDL_Quit();
            }

        private:
            bool initialized_ = false;
        };

        class Window final {
        public:
            void create(const std::string& title, const std::int32_t width, const std::int32_t height) {
                window_ = SDL_CreateWindow(title.c_str(), width, height,
                                           SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
                if (window_ == nullptr) throw std::runtime_error(SDL_GetError());
            }

            ~Window() {
                if (window_ != nullptr) SDL_DestroyWindow(window_);
            }

            [[nodiscard]] SDL_Window* get() const noexcept { return window_; }

        private:
            SDL_Window* window_ = nullptr;
        };

        explicit Impl(const RenderConfig &renderConfig) : renderer(renderConfig) {
        }

        // Destruction is intentionally reverse declaration order: renderer,
        // window, then SDL.  No lifecycle cleanup belongs to Application::run.
        SdlRuntime sdl;
        Window window;
        Renderer renderer;
        bool running = false;
        float fixedAccumulator = 0.0F;
        ScriptSystem scripts{ScriptRegistry::instance()};
    };

    Application::Application(ApplicationConfig config)
        : config_(std::move(config)), content_(config_.assetRoot),
          impl_(std::make_unique<Impl>(config_.render)) {
        if (config_.width <= 0 || config_.height <= 0) {
            throw std::invalid_argument("Application window dimensions must be positive");
        }
        if (config_.maxFrameDeltaTime <= 0.0F) {
            throw std::invalid_argument("Application max frame delta time must be positive");
        }
        if (config_.maxFixedStepsPerFrame == 0) {
            throw std::invalid_argument("Application must allow at least one fixed step per frame");
        }
        scene_.setContent(content_);
        initializeRuntime();
    }

    Application::~Application() = default;

    void Application::stop() const noexcept {
        impl_->running = false;
    }

    bool Application::isRunning() const noexcept {
        return impl_->running;
    }

    void Application::run() {
        if (game_ != nullptr) { game_->onStart(scene_); }
        impl_->running = true;
        while (impl_->running) {
            Renderer::beginFrame();
            processEvents();
            if (impl_->running) { updateFrame(static_cast<float>(Time::deltaTime())); }
        }
        if (game_ != nullptr) { game_->onShutdown(scene_); }
    }

    void Application::initializeRuntime() {
        impl_->sdl.initialize();
        impl_->window.create(config_.title, config_.width, config_.height);
        impl_->renderer.initialize(scene_, impl_->window.get());
    }

    void Application::processEvents() const {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            impl_->renderer.processEvent(&event);
            const bool closeRequested = event.type == SDL_EVENT_QUIT ||
                                        (config_.closeOnEscape && event.type == SDL_EVENT_KEY_DOWN &&
                                         event.key.key == SDLK_ESCAPE);
            if (closeRequested) { impl_->running = false; }
        }
    }

    void Application::updateFrame(const float deltaTime) {
        Profiler::beginFrame();
        {
            GE_PROFILE_SCOPE("UI");
            scene_.ui().update();
        }
        {
            GE_PROFILE_SCOPE("Game Update");
            if (game_ != nullptr) { game_->onUpdate(scene_, deltaTime); }
            if (updateCallback_) { updateCallback_(scene_, deltaTime); }
        }
        {
            GE_PROFILE_SCOPE("Scripts");
            impl_->scripts.update(scene_, deltaTime);
        }
        // Apply input-driven changes before stepping PhysX so movement and
        // jumping affect this frame rather than the next one.
        {
            GE_PROFILE_SCOPE("Physics");
            updatePhysics(deltaTime);
        }
        {
            GE_PROFILE_SCOPE("Renderer");
            impl_->renderer.renderFrame();
        }
        if (const auto gpu = impl_->renderer.gpuProfile()) {
            Profiler::setGpuFrameMilliseconds(gpu->frameMilliseconds);
        }
        Profiler::endFrame();
    }

    void Application::updatePhysics(const float deltaTime) {
        if (config_.fixedDeltaTime <= 0.0F) {
            scene_.physics().update(deltaTime);
            return;
        }

        const float clampedDeltaTime = std::clamp(deltaTime, 0.0F, config_.maxFrameDeltaTime);
        impl_->fixedAccumulator += clampedDeltaTime;
        std::uint32_t stepCount = 0;
        while (impl_->fixedAccumulator >= config_.fixedDeltaTime &&
               stepCount < config_.maxFixedStepsPerFrame) {
            if (game_ != nullptr) { game_->onFixedUpdate(scene_, config_.fixedDeltaTime); }
            scene_.physics().update(config_.fixedDeltaTime);
            impl_->fixedAccumulator -= config_.fixedDeltaTime;
            ++stepCount;
        }
        if (impl_->fixedAccumulator >= config_.fixedDeltaTime) {
            // Discard elapsed simulation time that cannot be caught up this
            // frame; retaining only interpolation remainder prevents a spiral.
            impl_->fixedAccumulator = std::fmod(impl_->fixedAccumulator, config_.fixedDeltaTime);
        }
    }
} // namespace Engine
