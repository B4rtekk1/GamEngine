#pragma once

#include "Engine/Core/Diagnostics.h"
#include "Engine/ECS/Registry.h"
#include "Engine/Scripting/ScriptRegistry.h"

#include <string>

namespace Engine {
    class Scene;
    struct ScriptComponent;

    /** Creates and executes native C++ scripts registered in ScriptRegistry. */
    class ScriptSystem final {
    public:
        explicit ScriptSystem(ScriptRegistry &scripts) noexcept : scripts_(scripts) {
        }

        void update(Registry &registry, float deltaTime) const;

        void update(Scene &scene, float deltaTime) const;

    private:
        void updateOne(Registry &registry, Entity entity, ScriptComponent &component,
                       float deltaTime, Scene *scene) const;
        [[nodiscard]] DiagnosticContext diagnosticContext(const Registry &registry, Entity entity,
                                                          const ScriptComponent &component,
                                                          std::string action) const;
        static void reportOnce(ScriptComponent &component, std::string key, DiagnosticSeverity severity,
                               std::string message, DiagnosticContext context);
        void destroyRuntime(const Registry &registry, Entity entity, ScriptComponent &component) const;
        void disableRuntime(const Registry &registry, Entity entity, ScriptComponent &component) const;
        [[nodiscard]] bool createRuntime(Registry &registry, Entity entity, ScriptComponent &component,
                                         Scene *scene) const;
        [[nodiscard]] bool enableRuntime(const Registry &registry, Entity entity, ScriptComponent &component) const;
        [[nodiscard]] bool updateRuntime(const Registry &registry, Entity entity, ScriptComponent &component,
                                         float deltaTime) const;

        ScriptRegistry &scripts_;
    };
} // namespace Engine
