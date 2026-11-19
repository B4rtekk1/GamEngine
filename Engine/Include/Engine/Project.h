#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

/**
 * Portable description of a GamEngine project.
 *
 * Paths stored in the manifest are relative to the manifest directory, so a
 * project can be moved without changing source code or build definitions.
 */
class Project final {
  public:
    /** Loads a `key = value` GamEngine project manifest. */
    [[nodiscard]] static Project load(const std::filesystem::path& manifestPath);

    /** Searches @p startDirectory and its parents for `GamEngine.project`. */
    [[nodiscard]] static Project discover(const std::filesystem::path& startDirectory);

    /**
     * Creates the built-in project configuration for a manifest-free game.
     * Assets are read from `Assets` and the startup scene is
     * `Assets/Scenes/Editor.scene`, both relative to @p rootDirectory.
     */
    [[nodiscard]] static Project defaults(const std::filesystem::path& rootDirectory);

    /**
     * Creates a standalone project directory and returns its manifest-backed
     * configuration. The directory must not already contain project files.
     * The created project contains only game-owned files; the engine executable
     * and its source tree remain external dependencies.
     */
    [[nodiscard]] static Project create(const std::filesystem::path& rootDirectory, std::string name = {});

    [[nodiscard]] const std::string& name() const noexcept {
        return name_;
    }
    [[nodiscard]] const std::filesystem::path& manifestPath() const noexcept {
        return manifestPath_;
    }
    [[nodiscard]] const std::filesystem::path& rootPath() const noexcept {
        return rootPath_;
    }
    [[nodiscard]] const std::filesystem::path& assetRoot() const noexcept {
        return assetRoot_;
    }
    [[nodiscard]] const std::filesystem::path& startupScene() const noexcept {
        return startupScene_;
    }

    void setName(std::string name) {
        name_ = std::move(name);
    }
    /** @p scene must be an absolute path (or resolved with resolve()). */
    void setStartupScene(std::filesystem::path scene) {
        startupScene_ = std::move(scene);
    }

    /** Plugins the project turned off (manifest key disabled_plugins). */
    [[nodiscard]] const std::vector<std::string>& disabledPlugins() const noexcept {
        return disabledPlugins_;
    }
    [[nodiscard]] bool pluginEnabled(std::string_view name) const;
    void setPluginEnabled(std::string_view name, bool enabled);

    /** Rewrites the manifest with the current name, asset root and startup scene. */
    void save() const;

    /** Returns every `.scene` file stored next to the startup scene. */
    [[nodiscard]] std::vector<std::filesystem::path> scenes() const;

    /** Resolves a project-relative path; absolute paths are returned unchanged. */
    [[nodiscard]] std::filesystem::path resolve(const std::filesystem::path& path) const;

  private:
    std::string name_;
    std::filesystem::path manifestPath_;
    std::filesystem::path rootPath_;
    std::filesystem::path assetRoot_;
    std::filesystem::path startupScene_;
    std::vector<std::string> disabledPlugins_;
};

} // namespace Engine
