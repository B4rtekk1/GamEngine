#include "Engine/Scripting/ScriptRegistry.h"

#include "Engine/Scripting/Script.h"

#include <utility>

namespace Engine {
    ScriptRegistry &ScriptRegistry::instance() {
        static ScriptRegistry registry;
        return registry;
    }

    RuntimeScriptInstance::RuntimeScriptInstance(RuntimeScriptInstance &&other) noexcept
        : instance(std::exchange(other.instance, nullptr)), destroy(std::exchange(other.destroy, nullptr)),
          moduleGeneration(std::exchange(other.moduleGeneration, 0)) {}

    RuntimeScriptInstance &RuntimeScriptInstance::operator=(RuntimeScriptInstance &&other) noexcept {
        if (this != &other) {
            reset();
            instance = std::exchange(other.instance, nullptr);
            destroy = std::exchange(other.destroy, nullptr);
            moduleGeneration = std::exchange(other.moduleGeneration, 0);
        }
        return *this;
    }

    void RuntimeScriptInstance::reset() noexcept {
        if (instance != nullptr && destroy != nullptr) destroy(instance);
        instance = nullptr;
        destroy = nullptr;
        moduleGeneration = 0;
    }

    std::uint64_t ScriptModuleRegistrar::generation() const noexcept { return registry_.activeRegistrationGeneration(); }

    void ScriptRegistry::registerClass(ScriptClassDescriptor descriptor) {
        if (descriptor.name.empty() || descriptor.create == nullptr || descriptor.destroy == nullptr) return;
        const std::string className = descriptor.name;
        classes_.insert_or_assign(className, std::move(descriptor));
        // RTTI for dynamically loaded types cannot safely outlive their module.
        // className<T>() is consequently only guaranteed for in-process registrations.
    }

    RuntimeScriptInstance ScriptRegistry::create(const std::string_view name) const {
        const auto found = classes_.find(std::string{name});
        if (found == classes_.end()) return {};
        const ScriptClassDescriptor &descriptor = found->second;
        RuntimeScriptInstance runtime;
        runtime.instance = descriptor.create();
        runtime.destroy = descriptor.destroy;
        runtime.moduleGeneration = descriptor.moduleGeneration;
        return runtime;
    }

    std::optional<std::string> ScriptRegistry::sourceFile(const std::string_view name) const {
        const auto found = classes_.find(std::string{name});
        if (found == classes_.end() || found->second.sourceFile.empty()) return std::nullopt;
        return found->second.sourceFile;
    }

    void ScriptRegistry::removeGeneration(const std::uint64_t generation) {
        std::erase_if(classes_, [generation](const auto &entry) { return entry.second.moduleGeneration == generation; });
    }
} // namespace Engine
