#include "Engine/Scripting/ScriptModuleManager.h"

#include "Engine/ECS/Components/ScriptComponent.h"
#include "Engine/ECS/Registry.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scripting/ScriptModuleApi.h"
#include "Engine/Scripting/ScriptRegistry.h"

namespace Engine {
    ScriptModuleManager::~ScriptModuleManager() = default;

    bool ScriptModuleManager::loadCandidate(const std::filesystem::path &path, LoadedModule &out) {
        const std::uint64_t generation = nextGeneration_;
        registry_.setActiveRegistrationGeneration(generation);
        if (!out.library.load(path)) {
            registry_.setActiveRegistrationGeneration(active_.generation);
            return false;
        }

        const auto version = out.library.symbol<decltype(&GE_GetScriptApiVersion)>("GE_GetScriptApiVersion");
        const auto registerScripts = out.library.symbol<decltype(&GE_RegisterGameScripts)>("GE_RegisterGameScripts");
        if (version == nullptr || registerScripts == nullptr || version() != ENGINE_SCRIPT_API_VERSION) {
            registry_.removeGeneration(generation);
            out.library.unload();
            registry_.setActiveRegistrationGeneration(active_.generation);
            return false;
        }

        ScriptModuleRegistrar registrar{registry_};
        registerScripts(&registrar);
        out.generation = generation;
        out.path = path;
        registry_.setActiveRegistrationGeneration(active_.generation);
        ++nextGeneration_;
        return true;
    }

    bool ScriptModuleManager::loadInitialModule(const std::filesystem::path &path) {
        if (active_.library.loaded()) return false;
        LoadedModule candidate;
        if (!loadCandidate(path, candidate)) return false;
        active_ = std::move(candidate);
        return true;
    }

    void ScriptModuleManager::destroyGeneration(Registry &scene, const std::uint64_t generation) const {
        scene.view<ScriptComponent>([generation](const Entity, ScriptComponent &component) {
            if (component.runtime.moduleGeneration == generation) component.reset();
        });
    }

    bool ScriptModuleManager::tryReload(const std::filesystem::path &candidatePath, Registry &scene) {
        LoadedModule candidate;
        if (!loadCandidate(candidatePath, candidate)) return false;

        // Candidate has passed all ABI checks. No code from active_ is unloaded
        // until every object using its vtable and deleter has been destroyed.
        destroyGeneration(scene, active_.generation);
        registry_.removeGeneration(active_.generation);
        active_.library.unload();
        active_ = std::move(candidate);
        return true;
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
