#include "Engine/Scripting/ScriptSystem.h"

#include "Engine/Core/Diagnostics.h"
#include "Engine/ECS/Components/ScriptComponent.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/Components/IdentityComponents.h"

#include <exception>
#include <vector>

namespace Engine {
    namespace {
        std::string objectName(const Registry &registry, const Entity entity) {
            if (registry.has<NameComponent>(entity)) {
                return registry.get<NameComponent>(entity).value;
            }
            return "Entity " + std::to_string(entity);
        }
    }

    DiagnosticContext ScriptSystem::diagnosticContext(const Registry &registry, const Entity entity,
                                                       const ScriptComponent &component, std::string action) const {
        return {
            .subsystem = "ScriptSystem",
            .object = objectName(registry, entity),
            .component = "ScriptComponent",
            .file = scripts_.sourceFile(component.className).value_or("Source file registration not found"),
            .suggestedAction = std::move(action),
        };
    }

    void ScriptSystem::reportOnce(ScriptComponent &component, std::string key, const DiagnosticSeverity severity,
                                  std::string message, DiagnosticContext context) {
        if (component.lastDiagnosticKey == key) {
            return;
        }
        component.lastDiagnosticKey = std::move(key);
        Diagnostics::instance().report(severity, std::move(message), std::move(context));
    }

    void ScriptSystem::destroyRuntime(const Registry &registry, const Entity entity, ScriptComponent &component) const {
        if (!component.runtime) {
            return;
        }
        if (component.runtimeEnabled) {
            try {
                component.runtime->onDisable();
            } catch (const std::exception &error) {
                reportOnce(component, "disable:" + component.runtimeClassName, DiagnosticSeverity::Error,
                           "Script " + component.runtimeClassName + " threw an exception during onDisable: " + error.what(),
                           diagnosticContext(registry, entity, component, "Fix onDisable or remove the script component."));
            } catch (...) {
                reportOnce(component, "disable:" + component.runtimeClassName, DiagnosticSeverity::Error,
                           "Script " + component.runtimeClassName + " threw an unknown exception during onDisable.",
                           diagnosticContext(registry, entity, component, "Fix onDisable or remove the script component."));
            }
            component.runtimeEnabled = false;
        }
        try {
            component.runtime->onDestroy();
        } catch (const std::exception &error) {
            reportOnce(component, "destroy:" + component.runtimeClassName, DiagnosticSeverity::Error,
                       "Script " + component.runtimeClassName + " threw an exception during onDestroy: " + error.what(),
                       diagnosticContext(registry, entity, component, "Fix onDestroy or remove the script component."));
        } catch (...) {
            reportOnce(component, "destroy:" + component.runtimeClassName, DiagnosticSeverity::Error,
                       "Script " + component.runtimeClassName + " threw an unknown exception during onDestroy.",
                       diagnosticContext(registry, entity, component, "Fix onDestroy or remove the script component."));
        }
        component.runtime.reset();
        component.runtimeClassName.clear();
    }

    void ScriptSystem::disableRuntime(const Registry &registry, const Entity entity, ScriptComponent &component) const {
        if (!component.runtime || !component.runtimeEnabled) {
            return;
        }
        try {
            component.runtime->onDisable();
            component.runtimeEnabled = false;
        } catch (const std::exception &error) {
            reportOnce(component, "disable:" + component.className, DiagnosticSeverity::Error,
                       "Script " + component.className + " threw an exception during onDisable: " + error.what(),
                       diagnosticContext(registry, entity, component, "Fix onDisable or remove the script component."));
        } catch (...) {
            reportOnce(component, "disable:" + component.className, DiagnosticSeverity::Error,
                       "Script " + component.className + " threw an unknown exception during onDisable.",
                       diagnosticContext(registry, entity, component, "Fix onDisable or remove the script component."));
        }
    }

    bool ScriptSystem::createRuntime(Registry &registry, const Entity entity, ScriptComponent &component,
                                     Scene *scene) const {
        component.runtime = scripts_.create(component.className);
        if (!component.runtime) {
            reportOnce(component, "missing:" + component.className, DiagnosticSeverity::Warning,
                       "Script " + component.className + " is not registered; register it or remove the component.",
                       diagnosticContext(registry, entity, component, "Register the script or remove the ScriptComponent."));
            return false;
        }
        component.runtimeClassName = component.className;
        component.lastDiagnosticKey.clear();
        try {
            if (scene != nullptr) {
                component.runtime->attach(*scene, registry, entity);
            } else {
                component.runtime->attach(registry, entity);
            }
            scripts_.applyFields(component.className, component.fields, *component.runtime.instance);
            component.runtime->onCreate();
            if (component.hasHotReloadState) {
                component.runtime->loadHotReloadState(component.hotReloadState);
                component.hotReloadState.clear();
                component.hasHotReloadState = false;
            }
            return true;
        } catch (const std::exception &error) {
            reportOnce(component, "create:" + component.className, DiagnosticSeverity::Error,
                       "Script " + component.className + " threw an exception during onCreate: " + error.what(),
                       diagnosticContext(registry, entity, component, "Fix onCreate or remove the script component."));
        } catch (...) {
            reportOnce(component, "create:" + component.className, DiagnosticSeverity::Error,
                       "Script " + component.className + " threw an unknown exception during onCreate.",
                       diagnosticContext(registry, entity, component, "Fix onCreate or remove the script component."));
        }
        destroyRuntime(registry, entity, component);
        return false;
    }

    bool ScriptSystem::enableRuntime(const Registry &registry, const Entity entity, ScriptComponent &component) const {
        if (component.runtimeEnabled) {
            return true;
        }
        try {
            component.runtime->onEnable();
            component.runtimeEnabled = true;
            return true;
        } catch (const std::exception &error) {
            reportOnce(component, "enable:" + component.className, DiagnosticSeverity::Error,
                       "Script " + component.className + " threw an exception during onEnable: " + error.what(),
                       diagnosticContext(registry, entity, component, "Fix onEnable or remove the script component."));
        } catch (...) {
            reportOnce(component, "enable:" + component.className, DiagnosticSeverity::Error,
                       "Script " + component.className + " threw an unknown exception during onEnable.",
                       diagnosticContext(registry, entity, component, "Fix onEnable or remove the script component."));
        }
        destroyRuntime(registry, entity, component);
        return false;
    }

    bool ScriptSystem::updateRuntime(const Registry &registry, const Entity entity, ScriptComponent &component,
                                     const float deltaTime) const {
        try {
            component.runtime->onUpdate(deltaTime);
            return true;
        } catch (const std::exception &error) {
            reportOnce(component, "update:" + component.className, DiagnosticSeverity::Error,
                       "Script " + component.className + " threw an exception during onUpdate: " + error.what(),
                       diagnosticContext(registry, entity, component, "Fix onUpdate or remove the script component."));
        } catch (...) {
            reportOnce(component, "update:" + component.className, DiagnosticSeverity::Error,
                       "Script " + component.className + " threw an unknown exception during onUpdate.",
                       diagnosticContext(registry, entity, component, "Fix onUpdate or remove the script component."));
        }
        destroyRuntime(registry, entity, component);
        return false;
    }

    void ScriptSystem::updateOne(Registry &registry, const Entity entity, ScriptComponent &component,
                                 const float deltaTime, Scene *scene) const {
        if (component.runtime && component.runtimeClassName != component.className) {
            destroyRuntime(registry, entity, component);
        }
        if (component.className.empty()) {
            return;
        }
        scripts_.syncFields(component);

        // Disabling preserves a script's runtime state. It will receive
        // onEnable on reactivation rather than being recreated.
        if (!component.enabled) {
            disableRuntime(registry, entity, component);
            return;
        }

        if (!component.runtime && !createRuntime(registry, entity, component, scene)) {
            return;
        }
        if (!enableRuntime(registry, entity, component)) {
            return;
        }
        (void) updateRuntime(registry, entity, component, deltaTime);
    }

    void ScriptSystem::update(Scene &scene, const float deltaTime) const {
        auto &registry = scene.registry();
        // A script is gameplay-facing code: it may destroy itself, spawn an
        // actor, or add/remove components.  Do not run it from a mutable view,
        // because Registry deliberately rejects those structural changes while
        // such a view is being iterated.  Capture the work list first, then
        // resolve every component again immediately before it is executed.
        std::vector<Entity> scriptedEntities;
        registry.view<ScriptComponent>([&](const Entity entity, const ScriptComponent &) {
            scriptedEntities.push_back(entity);
        });
        scene.beginScriptUpdateCommands();
        try {
            for (const Entity entity: scriptedEntities) {
                if (!registry.valid(entity) || !registry.has<ScriptComponent>(entity)) {
                    continue;
                }
                updateOne(registry, entity, registry.get<ScriptComponent>(entity), deltaTime, &scene);
            }
        } catch (...) {
            scene.flushScriptUpdateCommands();
            throw;
        }
        scene.flushScriptUpdateCommands();
    }

    void ScriptSystem::update(Registry &registry, const float deltaTime) const {
        std::vector<Entity> scriptedEntities;
        registry.view<ScriptComponent>([&](const Entity entity, const ScriptComponent &) {
            scriptedEntities.push_back(entity);
        });
        for (const Entity entity: scriptedEntities) {
            if (!registry.valid(entity) || !registry.has<ScriptComponent>(entity)) {
                continue;
            }
            updateOne(registry, entity, registry.get<ScriptComponent>(entity), deltaTime, nullptr);
        }
    }
} // namespace Engine
