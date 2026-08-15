#include "Engine/Renderer/Vulkan/renderer.h"
#include "Engine/Scene/Scene.h"

#include <cstdlib>
#include <exception>
#include <iostream>

int main() {
    try {
        Engine::Scene scene;
        Engine::Renderer renderer;
        renderer.run(scene);
    } catch (const std::exception& exception) {
        std::cerr << "Error: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
