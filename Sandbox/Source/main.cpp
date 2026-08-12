#include "Engine/Renderer/Vulkan/renderer.h"

#include <cstdlib>
#include <exception>
#include <iostream>

int main() {
    try {
        Engine::Renderer renderer;
        renderer.run();
    } catch (const std::exception& exception) {
        std::cerr << "Error: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
