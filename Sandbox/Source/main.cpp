#include <Engine/Engine.h>

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
    try {
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
                                            : [&] {
                                                  try {
                                                      return Engine::Project::discover(
                                                          std::filesystem::current_path());
                                                  } catch (const std::runtime_error&) {
                                                      const char* basePath = SDL_GetBasePath();
                                                      if (basePath == nullptr) {
                                                          throw std::runtime_error("Could not determine executable directory");
                                                      }
                                                      return Engine::Project::defaults(basePath);
                                                  }
                                              }();
        const std::filesystem::path scenePath = sceneOverride
                                                    ? project.resolve(*sceneOverride)
                                                    : project.startupScene();
        Engine::Application app{{.title = project.name(), .width = 800, .height = 600,
                                 .assetRoot = project.assetRoot()}};
        if (std::filesystem::is_regular_file(scenePath)) {
            app.scene().load(scenePath);
        } else {
            throw std::runtime_error("Scene file does not exist: " + scenePath.string());
        }
        app.run();
    } catch (const std::exception& exception) {
        std::cerr << "Error: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

// NOLINTEND(readability-magic-numbers)
