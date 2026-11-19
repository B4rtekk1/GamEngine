#pragma once

#include "Engine/Core/Diagnostics.h"
#include "Engine/Plugins/PluginSystem.h"
#include "Engine/Scripting/ScriptRegistry.h"

#include <concepts>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Engine {
/**
 * What a plugin may register during GE_PluginStartup. Everything registered
 * here is owned by the plugin's load: PluginManager releases it (script
 * instances, classes, systems) before the plugin library is unloaded, so a
 * plugin never has to unregister anything itself.
 *
 * The context stays valid for as long as the plugin is loaded, so systems
 * may keep a reference for name(), directory() and log(). Register scripts
 * and systems only inside GE_PluginStartup; later registrations are ignored.
 */
class PluginContext final {
  public:
    PluginContext(std::string name, std::filesystem::path directory, ScriptModuleRegistrar& scripts,
                  std::vector<std::unique_ptr<PluginSystem>>& systems) noexcept
        : name_(std::move(name)), directory_(std::move(directory)), scripts_(scripts), systems_(systems) {}

    PluginContext(const PluginContext&) = delete;
    PluginContext& operator=(const PluginContext&) = delete;

    /** The manifest's name. */
    [[nodiscard]] const std::string& name() const noexcept {
        return name_;
    }
    /** The plugin's folder, for its own files (Content/, configuration). */
    [[nodiscard]] const std::filesystem::path& directory() const noexcept {
        return directory_;
    }

    /**
     * Script classes the plugin provides; they appear in the Inspector's
     * script list next to the game's own scripts. Names must not clash with
     * already registered scripts.
     */
    [[nodiscard]] ScriptModuleRegistrar& scripts() noexcept {
        return scripts_;
    }

    /** Adds a game-wide system; see PluginSystem for when it runs. */
    void addSystem(std::unique_ptr<PluginSystem> system) {
        if (!system)
            return;
        if (sealed_) {
            log(DiagnosticSeverity::Warning, "addSystem() after GE_PluginStartup is ignored");
            return;
        }
        systems_.push_back(std::move(system));
    }
    /** Constructs and adds a system; returns nullptr after GE_PluginStartup. */
    template <typename T, typename... Arguments>
        requires std::derived_from<T, PluginSystem>
    T* addSystem(Arguments&&... arguments) {
        if (sealed_) {
            log(DiagnosticSeverity::Warning, "addSystem() after GE_PluginStartup is ignored");
            return nullptr;
        }
        auto system = std::make_unique<T>(std::forward<Arguments>(arguments)...);
        T* pointer = system.get();
        systems_.push_back(std::move(system));
        return pointer;
    }

    /** Writes to the engine log and the Editor console, tagged with the plugin name. */
    void log(const DiagnosticSeverity severity, std::string message) const {
        Diagnostics::instance().report(severity, "[" + name_ + "] " + std::move(message), {.subsystem = "Plugin"});
    }

  private:
    friend class PluginManager;
    // Called by PluginManager once GE_PluginStartup returns.
    void seal() noexcept {
        sealed_ = true;
    }

    std::string name_;
    std::filesystem::path directory_;
    ScriptModuleRegistrar& scripts_;
    std::vector<std::unique_ptr<PluginSystem>>& systems_;
    bool sealed_ = false;
};
} // namespace Engine
