#pragma once

#include "Engine/Project.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Editor {
    struct GameBuildSettings final {
        enum class Configuration { Release };

        std::filesystem::path outputDirectory;
        Configuration configuration = Configuration::Release;
        bool cleanBuild = true;
        bool includeDebugSymbols = false;
    };

    struct GameBuildResult final {
        bool success = false;
        std::filesystem::path executable;
        std::vector<std::string> warnings;
        std::vector<std::string> errors;
    };

    /** Assembles a self-contained Windows game from the Editor runtime template. */
    class GameBuildPipeline final {
    public:
        explicit GameBuildPipeline(std::filesystem::path editorRoot);
        [[nodiscard]] GameBuildResult build(const Engine::Project& project,
                                            const GameBuildSettings& settings) const;

    private:
        std::filesystem::path editorRoot_;
    };
} // namespace Editor
