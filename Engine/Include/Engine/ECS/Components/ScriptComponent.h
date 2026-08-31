#pragma once

#include "Engine/Scripting/Script.h"

#include <memory>
#include <string>

namespace Engine {
    /**
     * @brief Attaches a registered native C++ Script class to an entity.
     */
    struct ScriptComponent final {
        std::string className;
        bool enabled{true};

        ScriptComponent() = default;

        explicit ScriptComponent(std::string scriptClass, const bool active = true)
            : className(std::move(scriptClass)), enabled(active) {
        }

        ScriptComponent(const ScriptComponent &other)
            : className(other.className), enabled(other.enabled) {
        }

        ScriptComponent &operator=(const ScriptComponent &other) {
            if (this != &other) {
                reset();
                className = other.className;
                enabled = other.enabled;
            }
            return *this;
        }

        ScriptComponent(ScriptComponent &&) noexcept = default;

        ScriptComponent &operator=(ScriptComponent &&) noexcept = default;

        ~ScriptComponent() { reset(); }

        void reset() {
            if (runtime) {
                // Component removal can happen outside ScriptSystem (for example
                // through the editor). Lifecycle callbacks must never let an
                // exception escape a component destructor.
                try {
                    if (runtimeEnabled) runtime->onDisable();
                    runtime->onDestroy();
                } catch (...) {
                }
            }
            runtime.reset();
            runtimeClassName.clear();
            runtimeEnabled = false;
        }

    private:
        friend class ScriptSystem;
        std::unique_ptr<Script> runtime;
        std::string runtimeClassName;
        std::string lastDiagnosticKey;
        bool runtimeEnabled{false};
    };
} // namespace Engine
