#include <Engine/Engine.h>
#include <Engine/Core/Diagnostics.h>
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

int main(int argc, char** argv) {
    struct DiagnosticsGuard final {
        ~DiagnosticsGuard() { Engine::Diagnostics::instance().shutdown(); }
    };
    DiagnosticsGuard diagnosticsGuard;

    try {
        const char* const basePath = SDL_GetBasePath();
        if (basePath == nullptr) throw std::runtime_error("Could not determine executable directory");
        const std::filesystem::path executableRoot{basePath};
        std::filesystem::path executablePath = argc > 0 && argv[0] != nullptr
                                                   ? std::filesystem::path{argv[0]}
                                                   : executableRoot / "GamEnginePlayer.exe";
        std::string gameName = executablePath.stem().string();
        if (gameName.empty()) gameName = "Game";
        Engine::Diagnostics::instance().initialize(
            Platform::UserPaths::gameLogs(gameName), Engine::DiagnosticApplication::Game, gameName);

        std::optional<std::filesystem::path> projectPath;
        std::optional<std::filesystem::path> sceneOverride;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument{argv[index]};
            if (argument == "--project") {
                if (++index == argc) throw std::runtime_error("--project requires a file path");
                projectPath = argv[index];
                continue;
            }
            if (argument == "--scene") {
                if (++index == argc) throw std::runtime_error("--scene requires a file path");
                sceneOverride = std::filesystem::path{argv[index]};
            }
        }
        const Engine::Project project = projectPath
                                            ? Engine::Project::load(*projectPath)
                                            : Engine::Project::load(executableRoot / "GamEngine.project");
        const std::filesystem::path scenePath = sceneOverride
                                                    ? project.resolve(*sceneOverride)
                                                    : project.startupScene();
        Engine::ScriptModuleManager scriptModules{Engine::ScriptRegistry::instance()};
        const auto scriptModule = executableRoot / "GameScripts.dll";
        if (!std::filesystem::is_regular_file(scriptModule)) {
            throw std::runtime_error("Missing GameScripts.dll: " + scriptModule.string());
        }
        if (!scriptModules.loadInitialModule(scriptModule, Engine::ScriptModuleLoadMode::Direct)) {
            throw std::runtime_error("Could not load GameScripts.dll");
        }
        const Engine::RenderConfig renderConfig{
            .features = Engine::RenderFeatures{.shadows = true},
            .antialiasing = Engine::AntialiasingLevel::TAA,
            .shadowQuality = Engine::ShadowQuality::High,
            .gtaoQuality = Engine::GtaoQuality::High,
            .iblQuality = Engine::IblQuality::High,
        };
        Engine::Application app{{.title = project.name(), .width = 800, .height = 600,
                                 .closeOnEscape = false,
                                 .assetRoot = project.assetRoot(),
                                 .render = renderConfig}};
        if (!std::filesystem::is_regular_file(scenePath)) {
            throw std::runtime_error("Scene file does not exist: " + scenePath.string());
        }
        app.loadScene(scenePath);
        app.run();
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
