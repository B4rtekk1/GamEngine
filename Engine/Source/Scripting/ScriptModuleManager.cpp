#include "Engine/Scripting/ScriptModuleManager.h"

#include "Engine/ECS/Components/ScriptComponent.h"
#include "Engine/ECS/Registry.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scripting/ScriptModuleApi.h"
#include "Engine/Scripting/ScriptRegistry.h"

#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace Engine {
    ScriptModuleManager::~ScriptModuleManager() = default;

    std::filesystem::path ScriptModuleManager::makeVersionedCopy(const std::filesystem::path &path,
                                                                  const std::uint64_t generation) const {
        const auto directory = path.parent_path() / "HotReload";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) return {};

        std::ostringstream filename;
        filename << path.stem().string() << '_' << std::setfill('0') << std::setw(6) << generation
                 << path.extension().string();
        const auto versioned = directory / filename.str();
        std::filesystem::copy_file(path, versioned, std::filesystem::copy_options::overwrite_existing, error);
        return error ? std::filesystem::path{} : versioned;
    }

    bool ScriptModuleManager::loadCandidate(const std::filesystem::path &path, LoadedModule &out) {
        const std::uint64_t generation = nextGeneration_;
        const auto versionedPath = makeVersionedCopy(path, generation);
        if (versionedPath.empty() || !out.library.load(versionedPath)) return false;

        const auto version = out.library.symbol<decltype(&GE_GetScriptApiVersion)>("GE_GetScriptApiVersion");
        const auto registerScripts = out.library.symbol<decltype(&GE_RegisterGameScripts)>("GE_RegisterGameScripts");
        if (version == nullptr || registerScripts == nullptr || version() != ENGINE_SCRIPT_API_VERSION) {
            out.library.unload();
            return false;
        }

        ScriptModuleRegistrar registrar{generation};
        try {
            registerScripts(&registrar);
        } catch (...) {
            out.library.unload();
            return false;
        }
        std::unordered_set<std::string> names;
        for (const auto &descriptor : registrar.descriptors()) {
            if (descriptor.name.empty() || descriptor.create == nullptr || descriptor.destroy == nullptr ||
                descriptor.moduleGeneration != generation || !names.insert(descriptor.name).second) {
                out.library.unload();
                return false;
            }
        }
        out.generation = generation;
        out.path = versionedPath;
        out.descriptors = registrar.descriptors();
        ++nextGeneration_;
        return true;
    }

    bool ScriptModuleManager::loadInitialModule(const std::filesystem::path &path) {
        if (active_.library.loaded()) return false;
        LoadedModule candidate;
        if (!loadCandidate(path, candidate)) return false;
        for (auto &descriptor : candidate.descriptors) registry_.registerClass(std::move(descriptor));
        active_ = std::move(candidate);
        return true;
    }

    void ScriptModuleManager::destroyGeneration(Registry &scene, const std::uint64_t generation) const {
        scene.view<ScriptComponent>([generation](const Entity, ScriptComponent &component) {
            if (component.runtime.moduleGeneration == generation) {
                try {
                    component.hotReloadState = component.runtime->saveHotReloadState();
                    component.hasHotReloadState = true;
                } catch (...) {
                    component.hotReloadState.clear();
                    component.hasHotReloadState = false;
                }
                component.reset();
            }
        });
    }

    bool ScriptModuleManager::tryReload(const std::filesystem::path &candidatePath, Registry &scene) {
        LoadedModule candidate;
        if (!loadCandidate(candidatePath, candidate)) return false;

        // Candidate has passed all ABI checks. No code from active_ is unloaded
        // until every object using its vtable and deleter has been destroyed.
        destroyGeneration(scene, active_.generation);
        registry_.removeGeneration(active_.generation);
        for (auto &descriptor : candidate.descriptors) registry_.registerClass(std::move(descriptor));
        active_.library.unload();
        active_ = std::move(candidate);
        return true;
    }

    bool ScriptModuleManager::tryReload(const std::filesystem::path &candidatePath, Scene &scene) {
        return tryReload(candidatePath, scene.registry_);
    }

    void ScriptModuleManager::unload(Registry &scene) {
        destroyGeneration(scene, active_.generation);
        registry_.removeGeneration(active_.generation);
        active_.library.unload();
        active_.generation = 0;
        active_.path.clear();
    }

    void ScriptModuleManager::unload(Scene &scene) { unload(scene.registry_); }

    std::uint64_t ScriptModuleManager::activeGeneration() const noexcept { return active_.generation; }
} // namespace Engine
