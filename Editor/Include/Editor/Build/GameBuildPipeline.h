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
    /**
     * Ship only the startup scene and what it references: its sidecars, the
     * files its paths name and, for a mesh, the whole top-level asset folder
     * it lives in (where its cooked textures are). Off ships every asset, for
     * games whose scripts load other scenes.
     */
    bool startupSceneOnly = false;
};

struct GameBuildResult final {
    bool success = false;
    std::filesystem::path executable;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

/**
 * Removes @p buildDirectory when its CMake cache was generated from a
 * different source directory than @p sourceDirectory, so it can be
 * configured again. A project's script or plugin build tree is created by
 * whichever Editor opened it first; another Editor (a package, a different
 * build) has its SDK elsewhere and CMake refuses to reuse that cache.
 * Returns false when the directory could not be removed.
 */
bool resetBuildTreeForSource(const std::filesystem::path& buildDirectory, const std::filesystem::path& sourceDirectory);

/** Assembles a game for the host platform from the Editor runtime template. */
class GameBuildPipeline final {
  public:
    explicit GameBuildPipeline(std::filesystem::path editorRoot);
    [[nodiscard]] GameBuildResult build(const Engine::Project& project, const GameBuildSettings& settings) const;

  private:
    std::filesystem::path editorRoot_;
};
} // namespace Editor
