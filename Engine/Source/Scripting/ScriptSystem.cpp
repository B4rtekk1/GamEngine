#include "Engine/Scripting/ScriptSystem.h"
#include "Engine/Scene/Scene.h"

#include "Engine/ECS/Components/ScriptComponent.h"
#include "Engine/Core/Transform.h"

namespace Engine {
    void ScriptSystem::update(Scene &scene, const float deltaTime) const {
        auto &registry = scene.registry();
        registry.view<ScriptComponent>([&](const Entity entity, ScriptComponent &component) {
            if (!component.enabled || component.className.empty()) { return; }

            if (component.runtime && component.runtimeClassName != component.className) {
                component.runtime->onDestroy();
                component.runtime.reset();
            }
            if (!component.runtime) {
                component.runtime = scripts_.create(component.className);
                component.runtimeClassName = component.className;
                if (!component.runtime) { return; }
                component.runtime->attach(scene, registry, entity);
                component.runtime->onCreate();
            }
            component.runtime->onUpdate(deltaTime);
            if (registry.has<Transform>(entity)) { registry.markChanged<Transform>(entity); }
        });
    }

    void ScriptSystem::update(Registry &registry, const float deltaTime) const {
        registry.view<ScriptComponent>([&](const Entity entity, ScriptComponent &component) {
            if (!component.enabled || component.className.empty()) { return; }

            if (component.runtime && component.runtimeClassName != component.className) {
                component.runtime->onDestroy();
                component.runtime.reset();
            }
            if (!component.runtime) {
                component.runtime = scripts_.create(component.className);
                component.runtimeClassName = component.className;
                if (!component.runtime) { return; }
                component.runtime->attach(registry, entity);
                component.runtime->onCreate();
            }
            component.runtime->onUpdate(deltaTime);
            // Native scripts commonly mutate Transform through transform(). Mark
            // it dirty so render systems upload the new matrix on the next frame.
            if (registry.has<Transform>(entity)) {
                registry.markChanged<Transform>(entity);
            }
        });
    }
} // namespace Engine
