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
                runtime->onDestroy();
            }
            runtime.reset();
            runtimeClassName.clear();
        }

    private:
        friend class ScriptSystem;
        std::unique_ptr<Script> runtime;
        std::string runtimeClassName;
    };
} // namespace Engine
