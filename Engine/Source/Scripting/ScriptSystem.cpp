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
            if (registry.has<NameComponent>(entity)) return registry.get<NameComponent>(entity).value;
            return "Entity " + std::to_string(entity);
        }
    }

    void ScriptSystem::updateOne(Registry &registry, const Entity entity, ScriptComponent &component,
                                 const float deltaTime, Scene *scene) const {
        const auto context = [&](std::string action) {
            return DiagnosticContext{.subsystem = "ScriptSystem", .object = objectName(registry, entity),
                .component = "ScriptComponent", .file = scripts_.sourceFile(component.className).value_or(
                    "Source file registration not found"), .suggestedAction = std::move(action)};
        };
        const auto reportOnce = [&](std::string key, const DiagnosticSeverity severity,
                                    std::string message, DiagnosticContext diagnosticContext) {
            if (component.lastDiagnosticKey == key) return;
            component.lastDiagnosticKey = std::move(key);
            Diagnostics::instance().report(severity, std::move(message), std::move(diagnosticContext));
        };
        const auto destroyRuntime = [&] {
            if (!component.runtime) return;
            if (component.runtimeEnabled) {
                try {
                    component.runtime->onDisable();
                } catch (const std::exception &error) {
                    reportOnce("disable:" + component.runtimeClassName, DiagnosticSeverity::Error,
                        "Script " + component.runtimeClassName + " threw an exception during onDisable: " + error.what(),
                        context("Fix onDisable or remove the script component."));
                } catch (...) {
                    reportOnce("disable:" + component.runtimeClassName, DiagnosticSeverity::Error,
                        "Script " + component.runtimeClassName + " threw an unknown exception during onDisable.",
                        context("Fix onDisable or remove the script component."));
                }
                component.runtimeEnabled = false;
            }
            try {
                component.runtime->onDestroy();
            } catch (const std::exception &error) {
                reportOnce("destroy:" + component.runtimeClassName, DiagnosticSeverity::Error,
                    "Script " + component.runtimeClassName + " threw an exception during onDestroy: " + error.what(),
                    context("Fix onDestroy or remove the script component."));
            } catch (...) {
                reportOnce("destroy:" + component.runtimeClassName, DiagnosticSeverity::Error,
                    "Script " + component.runtimeClassName + " threw an unknown exception during onDestroy.",
                    context("Fix onDestroy or remove the script component."));
            }
            component.runtime.reset();
            component.runtimeClassName.clear();
        };

        if (component.runtime && component.runtimeClassName != component.className) destroyRuntime();
        if (component.className.empty()) return;
        scripts_.syncFields(component);

        // Disabling preserves a script's runtime state. It will receive
        // onEnable on reactivation rather than being recreated.
        if (!component.enabled) {
            if (component.runtime && component.runtimeEnabled) {
                try {
                    component.runtime->onDisable();
                    component.runtimeEnabled = false;
                } catch (const std::exception &error) {
                    reportOnce("disable:" + component.className, DiagnosticSeverity::Error,
                        "Script " + component.className + " threw an exception during onDisable: " + error.what(),
                        context("Fix onDisable or remove the script component."));
                } catch (...) {
                    reportOnce("disable:" + component.className, DiagnosticSeverity::Error,
                        "Script " + component.className + " threw an unknown exception during onDisable.",
                        context("Fix onDisable or remove the script component."));
                }
            }
            return;
        }

        if (!component.runtime) {
            component.runtime = scripts_.create(component.className);
            if (!component.runtime) {
                reportOnce("missing:" + component.className, DiagnosticSeverity::Warning,
                    "Script " + component.className + " is not registered; register it or remove the component.",
                    context("Register the script or remove the ScriptComponent."));
                return;
            }
            component.runtimeClassName = component.className;
            component.lastDiagnosticKey.clear();
            try {
                if (scene != nullptr) component.runtime->attach(*scene, registry, entity);
                else component.runtime->attach(registry, entity);
                scripts_.applyFields(component.className, component.fields, *component.runtime.instance);
                component.runtime->onCreate();
                if (component.hasHotReloadState) {
                    component.runtime->loadHotReloadState(component.hotReloadState);
                    component.hotReloadState.clear();
                    component.hasHotReloadState = false;
                }
            } catch (const std::exception &error) {
                reportOnce("create:" + component.className, DiagnosticSeverity::Error,
                    "Script " + component.className + " threw an exception during onCreate: " + error.what(),
                    context("Fix onCreate or remove the script component."));
                destroyRuntime();
                return;
            } catch (...) {
                reportOnce("create:" + component.className, DiagnosticSeverity::Error,
                    "Script " + component.className + " threw an unknown exception during onCreate.",
                    context("Fix onCreate or remove the script component."));
                destroyRuntime();
                return;
            }
        }
        if (!component.runtimeEnabled) {
            try {
                component.runtime->onEnable();
                component.runtimeEnabled = true;
            } catch (const std::exception &error) {
                reportOnce("enable:" + component.className, DiagnosticSeverity::Error,
                    "Script " + component.className + " threw an exception during onEnable: " + error.what(),
                    context("Fix onEnable or remove the script component."));
                destroyRuntime();
                return;
            } catch (...) {
                reportOnce("enable:" + component.className, DiagnosticSeverity::Error,
                    "Script " + component.className + " threw an unknown exception during onEnable.",
                    context("Fix onEnable or remove the script component."));
                destroyRuntime();
                return;
            }
        }
        try {
            component.runtime->onUpdate(deltaTime);
        } catch (const std::exception &error) {
            reportOnce("update:" + component.className, DiagnosticSeverity::Error,
                "Script " + component.className + " threw an exception during onUpdate: " + error.what(),
                context("Fix onUpdate or remove the script component."));
            destroyRuntime();
            return;
        } catch (...) {
            reportOnce("update:" + component.className, DiagnosticSeverity::Error,
                "Script " + component.className + " threw an unknown exception during onUpdate.",
                context("Fix onUpdate or remove the script component."));
            destroyRuntime();
            return;
        }
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
            for (const Entity entity : scriptedEntities) {
                if (!registry.valid(entity) || !registry.has<ScriptComponent>(entity)) continue;
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
        for (const Entity entity : scriptedEntities) {
            if (!registry.valid(entity) || !registry.has<ScriptComponent>(entity)) continue;
            updateOne(registry, entity, registry.get<ScriptComponent>(entity), deltaTime, nullptr);
        }
    }
} // namespace Engine
