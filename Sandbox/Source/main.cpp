#include "Engine/Renderer/Vulkan/renderer.h"
#include "Engine/Scene/Scene.h"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    try {
        const bool treeScene = argc > 1 && std::string_view{argv[1]} == "--tree";
        Engine::Scene scene(treeScene ? Engine::SceneType::Tree : Engine::SceneType::Cubes);
        Engine::Renderer renderer;
        renderer.run(scene);
    } catch (const std::exception& exception) {
        std::cerr << "Error: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
