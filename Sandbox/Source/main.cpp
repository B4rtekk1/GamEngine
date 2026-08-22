#include <Engine/Engine.h>
#include "Engine/Scene/SceneSerializer.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
    try {
        std::filesystem::path scenePath = std::filesystem::path{GAMEENGINE_SOURCE_DIR} /
                                          "Assets" / "Scenes" / "Editor.scene";
        Engine::Application app{{.title = "GamEngine", .width = 800, .height = 600}};
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument{argv[index]};
            if (argument == "--scene") {
                if (++index == argc) throw std::runtime_error("--scene requires a file path");
                scenePath = argv[index];
            }
        }
        if (std::filesystem::is_regular_file(scenePath)) {
            Engine::SceneSerializer::load(app.scene().registry, scenePath);
        } else if (argc > 1) {
            throw std::runtime_error("Scene file does not exist: " + scenePath.string());
        }
        app.run();
    } catch (const std::exception& exception) {
        std::cerr << "Error: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
