#include "Engine/Renderer/Vulkan/renderer.h"
#include "Engine/Scene/Scene.h"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    try {
        const std::string_view argument = argc > 1 ? std::string_view{argv[1]} : "";
        const Engine::SceneType sceneType = argument == "--tree"
            ? Engine::SceneType::Tree
            : argument == "--particles" ? Engine::SceneType::Particles
            : Engine::SceneType::Cubes;
        Engine::Scene scene(sceneType);
        Engine::Renderer renderer;
        renderer.run(scene);
    } catch (const std::exception& exception) {
        std::cerr << "Error: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
