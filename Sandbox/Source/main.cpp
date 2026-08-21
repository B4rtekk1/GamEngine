#include "Engine/Renderer/Vulkan/renderer.h"
#include "Engine/Core/Time.h"
#include "Engine/Scene/ScenePresets.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scripting/ScriptSystem.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include <SDL3/SDL.h>

int main(int argc, char** argv) {
    try {
        std::filesystem::path scenePath = std::filesystem::path{GAMEENGINE_SOURCE_DIR} /
                                          "Assets" / "Scenes" / "Editor.scene";
        std::string_view presetArgument;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument{argv[index]};
            if (argument == "--scene") {
                if (++index == argc) throw std::runtime_error("--scene requires a file path");
                scenePath = argv[index];
            } else {
                presetArgument = argument;
            }
        }
        const Engine::SceneType sceneType = presetArgument == "--tree"
            ? Engine::SceneType::Tree
            : presetArgument == "--cubes" ? Engine::SceneType::Cubes
            : Engine::SceneType::Particles;
        Engine::ScenePreset scene(sceneType);
        if (std::filesystem::is_regular_file(scenePath)) {
            Engine::SceneSerializer::load(scene.registry, scenePath);
        } else if (argc > 1) {
            throw std::runtime_error("Scene file does not exist: " + scenePath.string());
        }
        Engine::ScriptSystem scriptSystem{Engine::ScriptRegistry::instance()};
        Engine::Renderer renderer;
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_Window* window = SDL_CreateWindow("GamEngine", 800, 600,
                                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (window == nullptr) throw std::runtime_error(SDL_GetError());

        renderer.initialize(scene, window);
        bool running = true;
        while (running) {
            renderer.beginFrame();
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                renderer.processEvent(event);
                if (event.type == SDL_EVENT_QUIT ||
                    (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)) {
                    running = false;
                }
            }
            if (running) {
                renderer.renderFrame();
                scriptSystem.update(scene.registry, static_cast<float>(Engine::Time::deltaTime()));
            }
        }
        renderer.shutdown();
        SDL_DestroyWindow(window);
        SDL_Quit();
    } catch (const std::exception& exception) {
        std::cerr << "Error: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
