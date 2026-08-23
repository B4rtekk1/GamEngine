#include "Engine/Application.h"

#include "Engine/Core/Time.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Scripting/ScriptSystem.h"
#include "Engine/Physics/PhysicsSystem.h"

#include <SDL3/SDL.h>

#include <stdexcept>

namespace Engine {

class Application::Impl final {
public:
    explicit Impl(const RenderConfig& renderConfig) : renderer(renderConfig) {}
    Renderer renderer;
    SDL_Window* window = nullptr;
    bool initialized = false;
    bool running = false;
    ScriptSystem scripts{ScriptRegistry::instance()};
    PhysicsSystem physics{};
};

Application::Application(ApplicationConfig config)
    : config_(std::move(config)), impl_(std::make_unique<Impl>(config_.render)) {}

Application::~Application() {
    if (impl_->initialized) impl_->renderer.shutdown();
    if (impl_->window != nullptr) SDL_DestroyWindow(impl_->window);
    if (impl_->initialized) SDL_Quit();
}

void Application::stop() const noexcept {
    impl_->running = false;
}

bool Application::isRunning() const noexcept {
    return impl_->running;
}

void Application::run() {
    if (config_.width <= 0 || config_.height <= 0) {
        throw std::invalid_argument("Application window dimensions must be positive");
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        throw std::runtime_error(SDL_GetError());
    }
    impl_->initialized = true;
    impl_->window = SDL_CreateWindow(config_.title.c_str(), config_.width, config_.height,
                                     SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (impl_->window == nullptr) throw std::runtime_error(SDL_GetError());

    impl_->renderer.initialize(scene_, impl_->window);
    impl_->running = true;
    while (impl_->running) {
        impl_->renderer.beginFrame();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            impl_->renderer.processEvent(&event);
            if (event.type == SDL_EVENT_QUIT ||
                (config_.closeOnEscape && event.type == SDL_EVENT_KEY_DOWN &&
                 event.key.key == SDLK_ESCAPE)) {
                impl_->running = false;
            }
        }
        if (impl_->running) {
            if (updateCallback_) updateCallback_(scene_, static_cast<float>(Time::deltaTime()));
            impl_->physics.update(scene_, static_cast<float>(Time::deltaTime()));
            impl_->scripts.update(scene_, static_cast<float>(Time::deltaTime()));
            impl_->renderer.renderFrame();
        }
    }
    impl_->renderer.shutdown();
    impl_->initialized = false;
}

} // namespace Engine
