#include "Engine/Application.h"

#include "Engine/Core/Profiler.h"
#include "Engine/Core/Time.h"
#include "Engine/Physics/PhysicsSystem.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Scripting/ScriptSystem.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <stdexcept>

namespace Engine {
namespace {
struct ZoneSamples final {
    bool gpu{};
    std::uint16_t depth{};
    std::vector<double> milliseconds;
};

double percentile(std::vector<double>& values, const double fraction) {
    if (values.empty())
        return 0.0;
    std::ranges::sort(values);
    return values[static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1) + 0.5)];
}

double mean(const std::vector<double>& values) {
    double sum = 0.0;
    for (const double value : values)
        sum += value;
    return values.empty() ? 0.0 : sum / static_cast<double>(values.size());
}

/** Aggregates the profiler ring into per-zone statistics and writes them as CSV plus a stdout table. */
void writeBenchmarkReport(const std::vector<double>& wallFrameMs, const std::filesystem::path& csvPath) {
    std::map<std::string, ZoneSamples> zones;
    std::vector<double> cpuFrame;
    std::vector<double> gpuFrame;
    RenderGraphProfileStats graph{};
    std::uint32_t graphFrames = 0;
    for (std::uint32_t index = 0; index < Profiler::historySize(); ++index) {
        const ProfileFrame& frame = Profiler::historyFrame(index);
        cpuFrame.push_back(frame.cpuFrameMs);
        // Main-thread CPU zones only; summed so repeated zones in a frame count once.
        std::map<std::string, double> cpuSums;
        for (const CpuProfileEvent& event : frame.cpuEvents) {
            auto& zone = zones["CPU/" + std::string{Profiler::name(event.name)}];
            zone.depth = event.depth;
            cpuSums["CPU/" + std::string{Profiler::name(event.name)}] +=
                static_cast<double>(event.endNs - event.startNs) * 1.0e-6;
        }
        for (const auto& [name, value] : cpuSums)
            zones[name].milliseconds.push_back(value);
        if (!frame.gpuReady)
            continue;
        gpuFrame.push_back(frame.gpuFrameMs);
        std::map<std::string, double> gpuSums;
        for (const GpuProfileEvent& event : frame.gpuEvents) {
            const std::string key = "GPU/" + std::string{Profiler::name(event.name)};
            auto& zone = zones[key];
            zone.gpu = true;
            zone.depth = event.depth;
            gpuSums[key] += static_cast<double>(event.endMs - event.startMs);
        }
        for (const auto& [name, value] : gpuSums)
            zones[name].milliseconds.push_back(value);
        graph.pipelineBarriers += frame.renderGraph.pipelineBarriers;
        graph.imageBarriers += frame.renderGraph.imageBarriers;
        graph.passes += frame.renderGraph.passes;
        ++graphFrames;
    }
    std::ofstream csv;
    if (!csvPath.empty()) {
        csv.open(csvPath);
        csv << "name,depth,frames,mean_ms,p50_ms,p95_ms,max_ms\n";
    }
    const auto emit = [&](const std::string& name, std::uint16_t depth, std::vector<double> values) {
        const double meanMs = mean(values);
        const double p50 = percentile(values, 0.5);
        const double p95 = percentile(values, 0.95);
        const double maxMs = values.empty() ? 0.0 : values.back();
        std::printf("%-44s depth=%u n=%-4zu mean=%8.4f p50=%8.4f p95=%8.4f max=%8.4f ms\n", name.c_str(), depth,
                    values.size(), meanMs, p50, p95, maxMs);
        if (csv.is_open())
            csv << '"' << name << "\"," << depth << ',' << values.size() << ',' << meanMs << ',' << p50 << ','
                << p95 << ',' << maxMs << '\n';
    };
    std::printf("=== Benchmark report (%zu wall frames) ===\n", wallFrameMs.size());
    emit("Frame/wall", 0, wallFrameMs);
    emit("Frame/cpu", 0, cpuFrame);
    emit("Frame/gpu", 0, gpuFrame);
    if (graphFrames > 0)
        std::printf("RenderGraph per frame: passes=%.1f pipelineBarriers=%.1f imageBarriers=%.1f\n",
                    static_cast<double>(graph.passes) / graphFrames,
                    static_cast<double>(graph.pipelineBarriers) / graphFrames,
                    static_cast<double>(graph.imageBarriers) / graphFrames);
    for (auto& [name, zone] : zones)
        if (!zone.milliseconds.empty())
            emit(name, zone.depth, std::move(zone.milliseconds));
    std::fflush(stdout);
}
} // namespace

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
            if (initialized_) {
                SDL_Quit();
            }
        }

      private:
        bool initialized_ = false;
    };

    class Window final {
      public:
        void create(const std::string& title, const std::int32_t width, const std::int32_t height) {
            window_ = SDL_CreateWindow(title.c_str(), width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
            if (window_ == nullptr) {
                throw std::runtime_error(SDL_GetError());
            }
        }

        ~Window() {
            if (window_ != nullptr) {
                SDL_DestroyWindow(window_);
            }
        }

        [[nodiscard]] SDL_Window* get() const noexcept {
            return window_;
        }

      private:
        SDL_Window* window_ = nullptr;
    };

    explicit Impl(const RenderConfig& renderConfig) : renderer(renderConfig) {}

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
    : config_(std::move(config)), content_(config_.assetRoot), impl_(std::make_unique<Impl>(config_.render)) {
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

void Application::loadScene(const std::filesystem::path& path) {
    scene_.load(path);
    impl_->renderer.initializeScene(scene_);
}

void Application::run() {
    if (game_ != nullptr) {
        game_->onStart(scene_);
    }
    impl_->running = true;
    const bool benchmarking = config_.benchmarkFrames > 0;
    std::uint32_t frameIndex = 0;
    std::vector<double> wallFrameMs;
    auto previousFrame = std::chrono::steady_clock::now();
    while (impl_->running) {
        Renderer::beginFrame();
        processEvents();
        if (impl_->running) {
            updateFrame(static_cast<float>(Time::deltaTime()));
        }
        if (benchmarking) {
            const auto now = std::chrono::steady_clock::now();
            if (frameIndex == config_.benchmarkWarmupFrames) {
                Profiler::clear();
            } else if (frameIndex > config_.benchmarkWarmupFrames) {
                wallFrameMs.push_back(std::chrono::duration<double, std::milli>(now - previousFrame).count());
            }
            previousFrame = now;
            if (++frameIndex >= config_.benchmarkWarmupFrames + config_.benchmarkFrames + 3) {
                impl_->running = false;
            }
        }
    }
    if (benchmarking) {
        writeBenchmarkReport(wallFrameMs, config_.benchmarkOutput);
    }
    if (game_ != nullptr) {
        game_->onShutdown(scene_);
    }
}

void Application::initializeRuntime() {
    const auto assetRoot = content_.assetRoot();
    if (!assetRoot.empty()) {
        impl_->renderer.setProjectRoot(assetRoot.parent_path());
    }
    impl_->sdl.initialize();
    impl_->window.create(config_.title, config_.width, config_.height);
    impl_->renderer.initializeCore(scene_, impl_->window.get());
}

void Application::processEvents() const {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        impl_->renderer.processEvent(&event);
        const bool closeRequested =
            event.type == SDL_EVENT_QUIT ||
            (config_.closeOnEscape && event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE);
        if (closeRequested) {
            impl_->running = false;
        }
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
        if (game_ != nullptr) {
            game_->onUpdate(scene_, deltaTime);
        }
        if (updateCallback_) {
            updateCallback_(scene_, deltaTime);
        }
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
        Profiler::attachGpuFrame(gpu->frameNumber, gpu->frameMilliseconds, gpu->events);
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
    while (impl_->fixedAccumulator >= config_.fixedDeltaTime && stepCount < config_.maxFixedStepsPerFrame) {
        if (game_ != nullptr) {
            game_->onFixedUpdate(scene_, config_.fixedDeltaTime);
        }
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
