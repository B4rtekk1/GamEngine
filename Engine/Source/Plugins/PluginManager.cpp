#include "Engine/Plugins/PluginManager.h"

#include "Engine/Core/Diagnostics.h"
#include "Engine/ECS/Registry.h"
#include "Engine/Plugins/PluginApi.h"
#include "Engine/Plugins/PluginContext.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scripting/ScriptModuleManager.h"
#include "Engine/Scripting/ScriptRegistry.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <functional>
#include <ranges>
#include <unordered_map>

#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace Engine {
namespace {
[[nodiscard]] std::string trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0)
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0)
        value.remove_suffix(1);
    return std::string{value};
}

[[nodiscard]] std::vector<std::string> splitList(const std::string_view value) {
    std::vector<std::string> items;
    std::string_view remaining = value;
    while (!remaining.empty()) {
        const auto comma = remaining.find(',');
        if (std::string item = trim(remaining.substr(0, comma)); !item.empty())
            items.push_back(std::move(item));
        remaining = comma == std::string_view::npos ? std::string_view{} : remaining.substr(comma + 1);
    }
    return items;
}

// Names become folder, library and log names: keep them portable.
[[nodiscard]] bool validPluginName(const std::string_view name) {
    return !name.empty() && std::ranges::all_of(name, [](const char character) {
        return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_' || character == '-' ||
               character == '.';
    });
}

void report(const DiagnosticSeverity severity, std::string message) {
    Diagnostics::instance().report(severity, std::move(message), {.subsystem = "Plugins"});
}
} // namespace

const char* PluginManager::platformDirectory() noexcept {
#if defined(_WIN32)
    return "Win64";
#elif defined(__APPLE__)
    return "Mac";
#else
    return "Linux64";
#endif
}

PluginManager::PluginManager(ScriptRegistry& scripts) noexcept : scripts_(scripts) {}

PluginManager::~PluginManager() {
    unloadLibraries();
}

std::optional<PluginDescriptor> PluginManager::parseManifest(const std::filesystem::path& manifest,
                                                             std::string& error) {
    std::ifstream input{manifest};
    if (!input) {
        error = "Could not open the manifest";
        return std::nullopt;
    }
    std::unordered_map<std::string, std::string> values;
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed.starts_with('#') || trimmed.starts_with(';'))
            continue;
        const auto separator = trimmed.find('=');
        if (separator == std::string::npos) {
            error = "Line " + std::to_string(lineNumber) + " is not 'key = value'";
            return std::nullopt;
        }
        const std::string key = trim(std::string_view{trimmed}.substr(0, separator));
        if (key.empty() || !values.emplace(key, trim(std::string_view{trimmed}.substr(separator + 1))).second) {
            error = "Line " + std::to_string(lineNumber) + " repeats or misses a key";
            return std::nullopt;
        }
    }

    PluginDescriptor descriptor;
    descriptor.manifestPath = std::filesystem::absolute(manifest).lexically_normal();
    descriptor.directory = descriptor.manifestPath.parent_path();
    const auto value = [&](const char* key) -> std::string {
        const auto found = values.find(key);
        return found == values.end() ? std::string{} : found->second;
    };
    descriptor.name = value("name");
    if (!validPluginName(descriptor.name)) {
        error = descriptor.name.empty() ? "The manifest needs a name"
                                        : "Plugin names may only use letters, digits, '_', '-' and '.'";
        return std::nullopt;
    }
    const std::string apiVersion = value("api_version");
    try {
        std::size_t parsed = 0;
        const unsigned long number = std::stoul(apiVersion, &parsed);
        if (parsed != apiVersion.size())
            throw std::invalid_argument{"trailing characters"};
        descriptor.apiVersion = static_cast<std::uint32_t>(number);
    } catch (const std::exception&) {
        error = "The manifest needs a numeric api_version";
        return std::nullopt;
    }
    descriptor.version = value("version").empty() ? "0.0.0" : value("version");
    descriptor.description = value("description");
    if (const std::string type = value("type"); !type.empty())
        descriptor.type = type;
    descriptor.libraryName = value("library").empty() ? descriptor.name : value("library");
    if (!validPluginName(descriptor.libraryName)) {
        error = "Invalid library name '" + descriptor.libraryName + "'";
        return std::nullopt;
    }
    descriptor.dependencies = splitList(value("dependencies"));
    return descriptor;
}

std::vector<PluginStatus> PluginManager::discover(const std::vector<std::filesystem::path>& roots) {
    std::vector<PluginStatus> statuses;
    std::unordered_map<std::string, std::filesystem::path> firstLocation;
    for (const auto& root : roots) {
        std::error_code error;
        if (root.empty() || !std::filesystem::is_directory(root, error))
            continue;
        std::vector<std::filesystem::path> folders;
        for (const auto& entry : std::filesystem::directory_iterator{root, error})
            if (entry.is_directory())
                folders.push_back(entry.path());
        std::ranges::sort(folders);
        for (const auto& folder : folders) {
            std::vector<std::filesystem::path> manifests;
            for (const auto& entry : std::filesystem::directory_iterator{folder, error})
                if (entry.is_regular_file() && entry.path().extension() == ManifestExtension)
                    manifests.push_back(entry.path());
            std::ranges::sort(manifests);
            for (const auto& manifest : manifests) {
                PluginStatus status;
                std::string parseError;
                if (auto descriptor = parseManifest(manifest, parseError)) {
                    status.descriptor = std::move(*descriptor);
                    status.state = PluginState::Discovered;
                    const auto [first, inserted] =
                        firstLocation.try_emplace(status.descriptor.name, status.descriptor.directory);
                    if (!inserted) {
                        status.state = PluginState::Failed;
                        status.error = "Duplicate plugin name; " + first->second.string() + " is used instead";
                    }
                } else {
                    status.descriptor.name = manifest.stem().string();
                    status.descriptor.directory = folder;
                    status.descriptor.manifestPath = manifest;
                    status.state = PluginState::Failed;
                    status.error = parseError;
                }
                statuses.push_back(std::move(status));
            }
        }
    }
    return statuses;
}

std::filesystem::path PluginManager::findLibrary(const PluginDescriptor& descriptor) {
#if defined(_WIN32)
    const std::string filename = descriptor.libraryName + ".dll";
#elif defined(__APPLE__)
    const std::string filename = "lib" + descriptor.libraryName + ".dylib";
#else
    const std::string filename = "lib" + descriptor.libraryName + ".so";
#endif
    for (const auto& candidate :
         {descriptor.directory / "Binaries" / platformDirectory() / filename, descriptor.directory / filename}) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error))
            return candidate;
    }
    return {};
}

void PluginManager::loadAll(const std::vector<std::filesystem::path>& roots,
                            const std::unordered_set<std::string>& disabled) {
    if (!loaded_.empty()) {
        report(DiagnosticSeverity::Error, "loadAll() called while plugins are loaded; call unloadAll() first");
        return;
    }
    statuses_ = discover(roots);
    std::unordered_map<std::string, std::size_t> byName;
    // The first plugin of a name; discover() already failed later duplicates.
    for (std::size_t index = 0; index < statuses_.size(); ++index)
        byName.try_emplace(statuses_[index].descriptor.name, index);
    for (PluginStatus& status : statuses_)
        if (status.state == PluginState::Discovered && disabled.contains(status.descriptor.name))
            status.state = PluginState::Disabled;

    // Dependencies first; a plugin whose dependency is missing, disabled,
    // failed or cyclic fails with that reason.
    std::vector<std::uint8_t> visiting(statuses_.size(), 0);
    const std::function<bool(std::size_t)> visit = [&](const std::size_t index) -> bool {
        PluginStatus& status = statuses_[index];
        if (status.state == PluginState::Loaded)
            return true;
        if (status.state != PluginState::Discovered)
            return false;
        if (visiting[index] != 0) {
            status.state = PluginState::Failed;
            status.error = "Dependency cycle";
            return false;
        }
        visiting[index] = 1;
        for (const std::string& dependency : status.descriptor.dependencies) {
            const auto found = byName.find(dependency);
            if (found == byName.end()) {
                status.state = PluginState::Failed;
                status.error = "Requires plugin '" + dependency + "', which is not installed";
                return false;
            }
            if (!visit(found->second)) {
                if (status.state == PluginState::Discovered) {
                    status.state = PluginState::Failed;
                    status.error = "Requires plugin '" + dependency + "', which is not loaded";
                }
                return false;
            }
        }
        return load(status, index);
    };
    for (std::size_t index = 0; index < statuses_.size(); ++index)
        static_cast<void>(visit(index));

    for (const PluginStatus& status : statuses_) {
        if (status.state == PluginState::Loaded)
            report(DiagnosticSeverity::Info, "Loaded " + status.descriptor.name + " " + status.descriptor.version +
                                                 " (" + std::to_string(status.scriptCount) + " scripts, " +
                                                 std::to_string(status.systemCount) + " systems)");
        else if (status.state == PluginState::Failed)
            report(DiagnosticSeverity::Error, "Could not load " + status.descriptor.name + ": " + status.error);
    }
}

bool PluginManager::load(PluginStatus& status, const std::size_t statusIndex) {
    const PluginDescriptor& descriptor = status.descriptor;
    const auto fail = [&](std::string error) {
        status.state = PluginState::Failed;
        status.error = std::move(error);
        return false;
    };
    if (descriptor.type != "runtime")
        return fail("Plugin type '" + descriptor.type + "' is not supported yet; only 'runtime' plugins load");
    if (descriptor.apiVersion != ENGINE_PLUGIN_API_VERSION)
        return fail("The manifest targets plugin API " + std::to_string(descriptor.apiVersion) +
                    ", this engine provides " + std::to_string(ENGINE_PLUGIN_API_VERSION));
    status.libraryPath = findLibrary(descriptor);
    if (status.libraryPath.empty()) {
        std::error_code error;
        if (std::filesystem::is_directory(descriptor.directory / "Source", error))
            return fail(std::string{"No library found: not built yet. Build Source/ with the SDK's Plugin template "
                                    "(the Editor does this for project plugins)."});
        return fail(std::string{"No library found; expected Binaries/"} + platformDirectory() + "/ with " +
                    descriptor.libraryName);
    }

    auto plugin = std::make_unique<LoadedPlugin>();
    plugin->statusIndex = statusIndex;
    if (!plugin->library.load(status.libraryPath)) {
        std::string reason = "Could not load " + status.libraryPath.filename().string();
#ifndef _WIN32
        if (const char* detail = dlerror())
            reason += ": " + std::string{detail};
#endif
        return fail(std::move(reason));
    }
    const auto apiVersion = plugin->library.symbol<GE_GetPluginApiVersionFunction>("GE_GetPluginApiVersion");
    const auto startup = plugin->library.symbol<GE_PluginStartupFunction>("GE_PluginStartup");
    const auto shutdown = plugin->library.symbol<GE_PluginShutdownFunction>("GE_PluginShutdown");
    if (apiVersion == nullptr || startup == nullptr)
        return fail("The library does not export GE_GetPluginApiVersion and GE_PluginStartup");
    if (const std::uint32_t built = apiVersion(); built != ENGINE_PLUGIN_API_VERSION)
        return fail("The library was built for plugin API " + std::to_string(built) + ", this engine provides " +
                    std::to_string(ENGINE_PLUGIN_API_VERSION));

    plugin->generation = ScriptRegistry::allocateModuleGeneration();
    plugin->registrar = std::make_unique<ScriptModuleRegistrar>(plugin->generation);
    plugin->context =
        std::make_unique<PluginContext>(descriptor.name, descriptor.directory, *plugin->registrar, plugin->systems);
    const ScriptModuleRegistrar& registrar = *plugin->registrar;
    const auto abandon = [&](std::string error) {
        plugin->systems.clear();
        if (shutdown != nullptr) {
            try {
                shutdown();
            } catch (...) {
            }
        }
        return fail(std::move(error));
    };
    try {
        startup(plugin->context.get());
        plugin->context->seal();
    } catch (const std::exception& exception) {
        return abandon(std::string{"GE_PluginStartup threw: "} + exception.what());
    } catch (...) {
        return abandon("GE_PluginStartup threw an unknown exception");
    }

    std::unordered_set<std::string> names;
    for (const ScriptClassDescriptor& script : registrar.descriptors()) {
        if (script.name.empty() || script.create == nullptr || script.destroy == nullptr ||
            script.moduleGeneration != plugin->generation)
            return abandon("Registered an invalid script class");
        if (!names.insert(script.name).second)
            return abandon("Registers script '" + script.name + "' twice");
        if (scripts_.descriptor(script.name) != nullptr)
            return abandon("Script '" + script.name + "' is already provided by the game or another plugin");
    }
    for (ScriptClassDescriptor script : registrar.descriptors())
        scripts_.registerClass(std::move(script));

    status.state = PluginState::Loaded;
    status.error.clear();
    status.scriptCount = registrar.descriptors().size();
    status.systemCount = plugin->systems.size();
    loaded_.push_back(std::move(plugin));
    return true;
}

std::size_t PluginManager::loadedCount() const noexcept {
    return loaded_.size();
}

void PluginManager::forEachSystem(const char* callback, const auto& invoke) {
    for (const auto& plugin : loaded_) {
        if (plugin->systemsFailed)
            continue;
        for (const auto& system : plugin->systems) {
            try {
                invoke(*system);
            } catch (const std::exception& exception) {
                plugin->systemsFailed = true;
                report(DiagnosticSeverity::Error, statuses_[plugin->statusIndex].descriptor.name + " " + callback +
                                                      " threw: " + exception.what() + ". Its systems are stopped.");
                break;
            } catch (...) {
                plugin->systemsFailed = true;
                report(DiagnosticSeverity::Error, statuses_[plugin->statusIndex].descriptor.name + " " + callback +
                                                      " threw. Its systems are stopped.");
                break;
            }
        }
    }
}

void PluginManager::playStart(Scene& scene) {
    if (playing_)
        return;
    playing_ = true;
    for (const auto& plugin : loaded_)
        plugin->systemsFailed = false;
    forEachSystem("onPlayStart", [&](PluginSystem& system) { system.onPlayStart(scene); });
}

void PluginManager::update(Scene& scene, const float deltaTime) {
    if (playing_)
        forEachSystem("onUpdate", [&](PluginSystem& system) { system.onUpdate(scene, deltaTime); });
}

void PluginManager::fixedUpdate(Scene& scene, const float fixedDeltaTime) {
    if (playing_)
        forEachSystem("onFixedUpdate", [&](PluginSystem& system) { system.onFixedUpdate(scene, fixedDeltaTime); });
}

void PluginManager::playStop(Scene& scene) {
    if (!playing_)
        return;
    forEachSystem("onPlayStop", [&](PluginSystem& system) { system.onPlayStop(scene); });
    playing_ = false;
}

void PluginManager::unloadAll(Scene& scene) {
    playStop(scene);
    unloadAll(scene.registry());
}

void PluginManager::unloadAll(Registry& scene) {
    playing_ = false;
    for (const auto& plugin : std::views::reverse(loaded_))
        ScriptModuleManager::releaseGeneration(scripts_, scene, plugin->generation);
    unloadLibraries();
}

void PluginManager::unloadLibraries() noexcept {
    while (!loaded_.empty()) {
        std::unique_ptr<LoadedPlugin> plugin = std::move(loaded_.back());
        loaded_.pop_back();
        scripts_.removeGeneration(plugin->generation);
        // System destructors are plugin code: run them before the library goes.
        plugin->systems.clear();
        if (const auto shutdown = plugin->library.symbol<GE_PluginShutdownFunction>("GE_PluginShutdown")) {
            try {
                shutdown();
            } catch (...) {
                report(DiagnosticSeverity::Error,
                       "GE_PluginShutdown of " + statuses_[plugin->statusIndex].descriptor.name + " threw");
            }
        }
        plugin->context.reset();
        plugin->registrar.reset();
        plugin->library.unload();
        if (plugin->statusIndex < statuses_.size())
            statuses_[plugin->statusIndex].state = PluginState::Discovered;
    }
}
} // namespace Engine
