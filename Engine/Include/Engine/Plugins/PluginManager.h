#pragma once

#include "Engine/Plugins/PluginContext.h"
#include "Engine/Plugins/PluginSystem.h"
#include "Engine/Scripting/DynamicLibrary.h"
#include "Engine/Scripting/ScriptRegistry.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Engine {
class Registry;
class Scene;
class ScriptRegistry;

/**
 * A plugin's manifest, <Folder>/<Name>.geplugin, in the project manifest's
 * "key = value" format:
 *
 *     name = Weather
 *     version = 1.2.0
 *     description = Rain, snow and wind for outdoor scenes.
 *     api_version = 1
 *     type = runtime
 *     library = Weather              # optional, defaults to name
 *     dependencies = Wind, Clouds    # optional
 *
 * The library is looked up as Binaries/<Platform>/<prefix><library><suffix>
 * inside the plugin folder (libWeather.so in Binaries/Linux64, Weather.dll in
 * Binaries/Win64), then directly in the folder.
 */
struct PluginDescriptor final {
    std::string name;
    std::string version;
    std::string description;
    std::uint32_t apiVersion = 0;
    /// Only "runtime" plugins exist so far; "editor" is reserved.
    std::string type = "runtime";
    std::string libraryName;
    std::vector<std::string> dependencies;
    std::filesystem::path directory;
    std::filesystem::path manifestPath;
};

enum class PluginState {
    /// The manifest is valid; the plugin has not been loaded.
    Discovered,
    /// Discovered, turned off by the project.
    Disabled,
    /// Running: its scripts and systems are registered.
    Loaded,
    /// Discovered but could not be loaded; see PluginStatus::error.
    Failed,
};

struct PluginStatus final {
    PluginDescriptor descriptor;
    PluginState state = PluginState::Failed;
    std::string error;
    std::filesystem::path libraryPath;
    std::size_t scriptCount = 0;
    std::size_t systemCount = 0;
};

/** Discovers, loads and runs plugins. Main-thread only. */
class PluginManager final {
  public:
    static constexpr std::string_view ManifestExtension = ".geplugin";
    /// Binaries/<PlatformDirectory>/ holds a plugin's library.
    static const char* platformDirectory() noexcept;

    explicit PluginManager(ScriptRegistry& scripts) noexcept;
    /** Unloads whatever is still loaded; call unloadAll() first while a scene still holds plugin scripts. */
    ~PluginManager();
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    /**
     * Parses every <root>/<Folder>/<Folder or any>.geplugin. A plugin name
     * found in an earlier root wins; a later duplicate is reported as failed.
     * Manifests that do not parse are reported as failed too.
     */
    [[nodiscard]] static std::vector<PluginStatus> discover(const std::vector<std::filesystem::path>& roots);
    [[nodiscard]] static std::optional<PluginDescriptor> parseManifest(const std::filesystem::path& manifest,
                                                                       std::string& error);
    /** The library file a descriptor resolves to, or empty when none exists. */
    [[nodiscard]] static std::filesystem::path findLibrary(const PluginDescriptor& descriptor);

    /**
     * Discovers plugins in @p roots and loads every one not named in
     * @p disabled, dependencies first. Replaces whatever was loaded before
     * only after unloadAll().
     */
    void loadAll(const std::vector<std::filesystem::path>& roots, const std::unordered_set<std::string>& disabled = {});

    /**
     * Stops systems, destroys the plugins' script instances in @p scene,
     * removes their classes, calls GE_PluginShutdown and unloads every
     * library, in reverse load order.
     */
    void unloadAll(Registry& scene);
    void unloadAll(Scene& scene);

    [[nodiscard]] const std::vector<PluginStatus>& plugins() const noexcept {
        return statuses_;
    }
    [[nodiscard]] std::size_t loadedCount() const noexcept;

    // Game lifetime, forwarded to every loaded plugin's systems.
    void playStart(Scene& scene);
    void update(Scene& scene, float deltaTime);
    void fixedUpdate(Scene& scene, float fixedDeltaTime);
    void playStop(Scene& scene);
    [[nodiscard]] bool playing() const noexcept {
        return playing_;
    }

  private:
    struct LoadedPlugin final {
        // First, so it is destroyed last: everything below may hold plugin
        // code (vtables, std::function targets of script fields).
        DynamicLibrary library;
        std::size_t statusIndex = 0;
        std::uint64_t generation = 0;
        // The context outlives startup so systems may keep a reference to it.
        std::unique_ptr<ScriptModuleRegistrar> registrar;
        std::unique_ptr<PluginContext> context;
        std::vector<std::unique_ptr<PluginSystem>> systems;
        /// A system threw; this plugin's systems are not called again.
        bool systemsFailed = false;
    };

    [[nodiscard]] bool load(PluginStatus& status, std::size_t statusIndex);
    void forEachSystem(const char* callback, const auto& invoke);
    void unloadLibraries() noexcept;

    ScriptRegistry& scripts_;
    std::vector<PluginStatus> statuses_;
    // Load order; unloaded in reverse.
    std::vector<std::unique_ptr<LoadedPlugin>> loaded_;
    bool playing_ = false;
};
} // namespace Engine
