#include "Platform/RuntimePaths.h"

#include <Engine/Core/Diagnostics.h>
#include <Engine/Engine.h>
#include <Engine/Plugins/PluginManager.h>
#include <Engine/Scripting/ScriptModuleManager.h>
#include <Engine/Scripting/ScriptRegistry.h>
#include <Platform/UserPaths.h>
#include <SDL3/SDL.h>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

// NOLINTBEGIN(readability-magic-numbers)

namespace {
// Runs the plugins' systems for the whole life of the game.
class PluginSystems final : public Engine::Game {
  public:
    explicit PluginSystems(Engine::PluginManager& plugins) noexcept : plugins_(plugins) {}
    void onStart(Engine::Scene& scene) override {
        plugins_.playStart(scene);
    }
    void onUpdate(Engine::Scene& scene, const float deltaTime) override {
        plugins_.update(scene, deltaTime);
    }
    void onFixedUpdate(Engine::Scene& scene, const float deltaTime) override {
        plugins_.fixedUpdate(scene, deltaTime);
    }
    void onShutdown(Engine::Scene& scene) override {
        plugins_.playStop(scene);
    }

  private:
    Engine::PluginManager& plugins_;
};
} // namespace

int main(int argc, char** argv) {
    struct DiagnosticsGuard final {
        ~DiagnosticsGuard() {
            Engine::Diagnostics::instance().shutdown();
        }
    };
    DiagnosticsGuard diagnosticsGuard;

    try {
        const char* const basePath = SDL_GetBasePath();
        if (basePath == nullptr)
            throw std::runtime_error("Could not determine executable directory");
        const std::filesystem::path executableRoot{basePath};
        std::filesystem::path executablePath =
            argc > 0 && argv[0] != nullptr ? std::filesystem::path{argv[0]} : executableRoot / Platform::PlayerFilename;
        std::string gameName = executablePath.stem().string();
        if (gameName.empty())
            gameName = "Game";
        Engine::Diagnostics::instance().initialize(Platform::UserPaths::gameLogs(gameName),
                                                   Engine::DiagnosticApplication::Game, gameName);

        std::optional<std::filesystem::path> projectPath;
        std::optional<std::filesystem::path> sceneOverride;
        int windowWidth = 800;
        int windowHeight = 600;
        std::uint32_t benchmarkFrames = 0;
        std::uint32_t benchmarkWarmup = 120;
        std::filesystem::path benchmarkOutput;
        std::filesystem::path screenshotPath;
        std::uint32_t screenshotFrame = 240;
        // The last physics step of a frame runs while
        // the frame renders; --no-pipelined-physics applies each step before
        // rendering instead, one fixed step sooner.
        bool pipelinedPhysics = true;
        // --sequential-compute keeps every compute pass on the graphics queue,
        // so each one gets its own isolated GPU timestamp when profiling.
        bool asyncCompute = true;
        // --no-mesh-shaders draws GPU-culled meshlets as indexed draws, for
        // A/B measurements against the mesh-shader path.
        bool meshShaders = true;
        // --local-shadows auto|nvidia|generic picks the ray tracing scene of
        // local light shadows; nvidia falls back to generic on other GPUs.
        Engine::LocalShadowBackend localShadowBackend = Engine::LocalShadowBackend::Auto;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument{argv[index]};
            if (argument == "--no-pipelined-physics") {
                pipelinedPhysics = false;
                continue;
            }
            if (argument == "--sequential-compute") {
                asyncCompute = false;
                continue;
            }
            if (argument == "--local-shadows") {
                if (++index == argc)
                    throw std::runtime_error("--local-shadows requires auto, nvidia or generic");
                const std::string_view value{argv[index]};
                if (value == "auto")
                    localShadowBackend = Engine::LocalShadowBackend::Auto;
                else if (value == "nvidia")
                    localShadowBackend = Engine::LocalShadowBackend::Nvidia;
                else if (value == "generic")
                    localShadowBackend = Engine::LocalShadowBackend::Generic;
                else
                    throw std::runtime_error("--local-shadows requires auto, nvidia or generic");
                continue;
            }
            if (argument == "--no-mesh-shaders") {
                meshShaders = false;
                continue;
            }
            if (argument == "--project") {
                if (++index == argc)
                    throw std::runtime_error("--project requires a file path");
                projectPath = argv[index];
                continue;
            }
            if (argument == "--scene") {
                if (++index == argc)
                    throw std::runtime_error("--scene requires a file path");
                sceneOverride = std::filesystem::path{argv[index]};
                continue;
            }
            if (argument == "--width" || argument == "--height") {
                if (++index == argc)
                    throw std::runtime_error(std::string{argument} + " requires a value");
                (argument == "--width" ? windowWidth : windowHeight) = std::stoi(argv[index]);
                continue;
            }
            if (argument == "--screenshot" || argument == "--screenshot-frame") {
                if (++index == argc)
                    throw std::runtime_error(std::string{argument} + " requires a value");
                if (argument == "--screenshot")
                    screenshotPath = argv[index];
                else
                    screenshotFrame = static_cast<std::uint32_t>(std::stoul(argv[index]));
                continue;
            }
            if (argument == "--benchmark" || argument == "--benchmark-warmup" || argument == "--benchmark-output") {
                if (++index == argc)
                    throw std::runtime_error(std::string{argument} + " requires a value");
                if (argument == "--benchmark")
                    benchmarkFrames = static_cast<std::uint32_t>(std::stoul(argv[index]));
                else if (argument == "--benchmark-warmup")
                    benchmarkWarmup = static_cast<std::uint32_t>(std::stoul(argv[index]));
                else
                    benchmarkOutput = argv[index];
            }
        }
        const Engine::Project project = projectPath ? Engine::Project::load(*projectPath)
                                                    : Engine::Project::load(executableRoot / "GamEngine.project");
        const std::filesystem::path scenePath =
            sceneOverride ? project.resolve(*sceneOverride) : project.startupScene();
        Engine::ScriptModuleManager scriptModules{Engine::ScriptRegistry::instance()};
        // Outlives the Application below: plugin scripts in its scene run
        // plugin code while being destroyed.
        Engine::PluginManager plugins{Engine::ScriptRegistry::instance()};
        const auto scriptModule = executableRoot / Platform::ScriptModuleFilename;
        if (!std::filesystem::is_regular_file(scriptModule)) {
            throw std::runtime_error("Missing game script module: " + scriptModule.string());
        }
        if (!scriptModules.loadInitialModule(scriptModule, Engine::ScriptModuleLoadMode::Direct)) {
            throw std::runtime_error("Could not load game script module");
        }
        // A built game ships only the plugins its project enabled.
        plugins.loadAll({executableRoot / "Plugins"});
        const Engine::RenderConfig renderConfig{
            // Hi-Z culling rides on the shadow hierarchy that High VSM builds
            // anyway; keep the explicit flag so a game can opt out per title.
            .features = Engine::RenderFeatures{.shadows = true, .occlusionCulling = true, .meshShaders = meshShaders},
            .renderPath = asyncCompute ? Engine::RenderPath::AsyncCompute : Engine::RenderPath::Sequential,
            .antialiasing = Engine::AntialiasingLevel::TAA,
            .shadowQuality = Engine::ShadowQuality::High,
            .localShadowBackend = localShadowBackend,
            .gtaoQuality = Engine::GtaoQuality::High,
            .iblQuality = Engine::IblQuality::High,
        };
        Engine::Application app{{.title = project.name(),
                                 .width = windowWidth,
                                 .height = windowHeight,
                                 .closeOnEscape = false,
                                 .pipelinedPhysics = pipelinedPhysics,
                                 .assetRoot = project.assetRoot(),
                                 .render = renderConfig,
                                 .benchmarkFrames = benchmarkFrames,
                                 .benchmarkWarmupFrames = benchmarkWarmup,
                                 .benchmarkOutput = benchmarkOutput,
                                 .screenshotPath = screenshotPath,
                                 .screenshotFrame = screenshotFrame}};
        if (!std::filesystem::is_regular_file(scenePath)) {
            throw std::runtime_error("Scene file does not exist: " + scenePath.string());
        }
        PluginSystems pluginSystems{plugins};
        app.setGame(pluginSystems);
        app.loadScene(scenePath);
        app.run();
        plugins.unloadAll(app.scene());
        scriptModules.unload(app.scene());
    } catch (const std::exception& exception) {
        auto& diagnostics = Engine::Diagnostics::instance();
        diagnostics.report(Engine::DiagnosticSeverity::Error, exception.what(), {.subsystem = "Player"});
        diagnostics.flush();
        std::cerr << "Fatal error: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

// NOLINTEND(readability-magic-numbers)
