#include "Engine/Renderer/Vulkan/renderer.h"
#include "Engine/Scene/ScenePresets.h"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string_view>

#include <SDL3/SDL.h>

int main(int argc, char** argv) {
    try {
        const std::string_view argument = argc > 1 ? std::string_view{argv[1]} : "";
        const Engine::SceneType sceneType = argument == "--tree"
            ? Engine::SceneType::Tree
            : argument == "--particles" ? Engine::SceneType::Particles
            : Engine::SceneType::Cubes;
        Engine::ScenePreset scene(sceneType);
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
            if (running) renderer.renderFrame();
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
