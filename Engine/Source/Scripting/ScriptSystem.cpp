#include "Engine/Scripting/ScriptSystem.h"

#include "Engine/Core/Diagnostics.h"
#include "Engine/Core/Transform.h"
#include "Engine/ECS/Components/ScriptComponent.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/Components/IdentityComponents.h"

#include <exception>

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
                    "Nie znaleziono rejestracji pliku"), .suggestedAction = std::move(action)};
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
                        "Skrypt " + component.runtimeClassName + " zgłosił wyjątek podczas onDisable: " + error.what(),
                        context("Popraw onDisable lub usuń komponent skryptu."));
                } catch (...) {
                    reportOnce("disable:" + component.runtimeClassName, DiagnosticSeverity::Error,
                        "Skrypt " + component.runtimeClassName + " zgłosił nieznany wyjątek podczas onDisable.",
                        context("Popraw onDisable lub usuń komponent skryptu."));
                }
                component.runtimeEnabled = false;
            }
            try {
                component.runtime->onDestroy();
            } catch (const std::exception &error) {
                reportOnce("destroy:" + component.runtimeClassName, DiagnosticSeverity::Error,
                    "Skrypt " + component.runtimeClassName + " zgłosił wyjątek podczas onDestroy: " + error.what(),
                    context("Popraw onDestroy lub usuń komponent skryptu."));
            } catch (...) {
                reportOnce("destroy:" + component.runtimeClassName, DiagnosticSeverity::Error,
                    "Skrypt " + component.runtimeClassName + " zgłosił nieznany wyjątek podczas onDestroy.",
                    context("Popraw onDestroy lub usuń komponent skryptu."));
            }
            component.runtime.reset();
            component.runtimeClassName.clear();
        };

        if (component.runtime && component.runtimeClassName != component.className) destroyRuntime();
        if (component.className.empty()) return;

        // Disabling preserves a script's runtime state. It will receive
        // onEnable on reactivation rather than being recreated.
        if (!component.enabled) {
            if (component.runtime && component.runtimeEnabled) {
                try {
                    component.runtime->onDisable();
                    component.runtimeEnabled = false;
                } catch (const std::exception &error) {
                    reportOnce("disable:" + component.className, DiagnosticSeverity::Error,
                        "Skrypt " + component.className + " zgłosił wyjątek podczas onDisable: " + error.what(),
                        context("Popraw onDisable lub usuń komponent skryptu."));
                } catch (...) {
                    reportOnce("disable:" + component.className, DiagnosticSeverity::Error,
                        "Skrypt " + component.className + " zgłosił nieznany wyjątek podczas onDisable.",
                        context("Popraw onDisable lub usuń komponent skryptu."));
                }
            }
            return;
        }

        if (!component.runtime) {
            component.runtime = scripts_.create(component.className);
            if (!component.runtime) {
                reportOnce("missing:" + component.className, DiagnosticSeverity::Warning,
                    "Brak skryptu " + component.className + "; zarejestruj go lub usuń komponent.",
                    context("Zarejestruj skrypt lub usuń komponent ScriptComponent."));
                return;
            }
            component.runtimeClassName = component.className;
            component.lastDiagnosticKey.clear();
            try {
                if (scene != nullptr) component.runtime->attach(*scene, registry, entity);
                else component.runtime->attach(registry, entity);
                component.runtime->onCreate();
            } catch (const std::exception &error) {
                reportOnce("create:" + component.className, DiagnosticSeverity::Error,
                    "Skrypt " + component.className + " zgłosił wyjątek podczas onCreate: " + error.what(),
                    context("Popraw onCreate lub usuń komponent skryptu."));
                destroyRuntime();
                return;
            } catch (...) {
                reportOnce("create:" + component.className, DiagnosticSeverity::Error,
                    "Skrypt " + component.className + " zgłosił nieznany wyjątek podczas onCreate.",
                    context("Popraw onCreate lub usuń komponent skryptu."));
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
                    "Skrypt " + component.className + " zgłosił wyjątek podczas onEnable: " + error.what(),
                    context("Popraw onEnable lub usuń komponent skryptu."));
                destroyRuntime();
                return;
            } catch (...) {
                reportOnce("enable:" + component.className, DiagnosticSeverity::Error,
                    "Skrypt " + component.className + " zgłosił nieznany wyjątek podczas onEnable.",
                    context("Popraw onEnable lub usuń komponent skryptu."));
                destroyRuntime();
                return;
            }
        }
        try {
            component.runtime->onUpdate(deltaTime);
        } catch (const std::exception &error) {
            reportOnce("update:" + component.className, DiagnosticSeverity::Error,
                "Skrypt " + component.className + " zgłosił wyjątek podczas onUpdate: " + error.what(),
                context("Popraw onUpdate lub usuń komponent skryptu."));
            destroyRuntime();
            return;
        } catch (...) {
            reportOnce("update:" + component.className, DiagnosticSeverity::Error,
                "Skrypt " + component.className + " zgłosił nieznany wyjątek podczas onUpdate.",
                context("Popraw onUpdate lub usuń komponent skryptu."));
            destroyRuntime();
            return;
        }
        if (registry.has<Transform>(entity)) registry.markChanged<Transform>(entity);
    }

    void ScriptSystem::update(Scene &scene, const float deltaTime) const {
        auto &registry = scene.registry();
        registry.view<ScriptComponent>([&](const Entity entity, ScriptComponent &component) {
            updateOne(registry, entity, component, deltaTime, &scene);
        });
    }

    void ScriptSystem::update(Registry &registry, const float deltaTime) const {
        registry.view<ScriptComponent>([&](const Entity entity, ScriptComponent &component) {
            updateOne(registry, entity, component, deltaTime, nullptr);
        });
    }
} // namespace Engine
