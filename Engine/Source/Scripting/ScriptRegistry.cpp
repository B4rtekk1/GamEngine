#include "Engine/Scripting/ScriptRegistry.h"

#include "Engine/Scripting/Script.h"
#include "Engine/ECS/Components/ScriptComponent.h"

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

    const ScriptClassDescriptor *ScriptRegistry::descriptor(const std::string_view name) const {
        const auto found = classes_.find(std::string{name});
        return found == classes_.end() ? nullptr : &found->second;
    }

    void ScriptRegistry::syncFields(ScriptComponent &component) const {
        const auto *classDescriptor = descriptor(component.className);
        if (classDescriptor == nullptr) return;
        std::map<std::string, ScriptFieldValue> synchronized;
        for (const auto &field : classDescriptor->fields) {
            const auto found = component.fields.find(field.name);
            if (found != component.fields.end() && found->second.index() == field.defaultValue.index()) {
                synchronized[field.name] = found->second;
                continue;
            }
            bool migrated = false;
            for (const auto &attribute : field.attributes) {
                const auto *former = std::get_if<ScriptFormerlySerializedAsAttribute>(&attribute);
                if (former == nullptr) continue;
                const auto old = component.fields.find(former->name);
                if (old != component.fields.end() && old->second.index() == field.defaultValue.index()) {
                    synchronized[field.name] = old->second;
                    migrated = true;
                    break;
                }
            }
            if (!migrated) synchronized[field.name] = field.defaultValue;
        }
        component.fields = std::move(synchronized);
    }

    void ScriptRegistry::applyFields(const std::string_view name,
                                     const std::map<std::string, ScriptFieldValue> &values,
                                     Script &script) const {
        const auto *classDescriptor = descriptor(name);
        if (classDescriptor == nullptr) return;
        for (const auto &field : classDescriptor->fields) {
            const auto found = values.find(field.name);
            if (found != values.end() && field.write != nullptr) field.write(&script, found->second);
        }
    }

    void ScriptRegistry::captureFields(const std::string_view name, const Script &script,
                                       std::map<std::string, ScriptFieldValue> &values) const {
        const auto *classDescriptor = descriptor(name);
        if (classDescriptor == nullptr) return;
        std::map<std::string, ScriptFieldValue> captured;
        for (const auto &field : classDescriptor->fields) {
            if (field.read == nullptr) continue;
            ScriptFieldValue value = field.defaultValue;
            field.read(&script, value);
            captured.emplace(field.name, std::move(value));
        }
        values = std::move(captured);
    }

    void ScriptRegistry::removeGeneration(const std::uint64_t generation) {
        std::erase_if(classes_, [generation](const auto &entry) { return entry.second.moduleGeneration == generation; });
    }
} // namespace Engine
