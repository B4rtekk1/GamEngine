#pragma once

#include "Engine/Scripting/Script.h"

#include <functional>
#include <concepts>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace Engine {

/** Registry of C++ script classes available to ScriptComponent. */
class ScriptRegistry final {
public:
    using Factory = std::function<std::unique_ptr<Script>()>;
    [[nodiscard]] static ScriptRegistry& instance() {
        static auto* registry = new ScriptRegistry();
        return *registry;
    }

    template<typename T>
    void registerClass(std::string name) {
        static_assert(std::derived_from<T, Script>);
        const std::string className = name;
        factories_.insert_or_assign(className, [] { return std::make_unique<T>(); });
        typeNames_.insert_or_assign(std::type_index(typeid(T)), std::move(name));
    }

    template<typename T>
    [[nodiscard]] std::optional<std::string> className() const {
        static_assert(std::derived_from<T, Script>);
        const auto found = typeNames_.find(std::type_index(typeid(T)));
        if (found == typeNames_.end()) return std::nullopt;
        return found->second;
    }

    [[nodiscard]] std::unique_ptr<Script> create(const std::string_view name) const {
        const auto found = factories_.find(std::string{name});
        return found == factories_.end() ? nullptr : found->second();
    }

    [[nodiscard]] std::vector<std::string> classNames() const {
        std::vector<std::string> names;
        names.reserve(factories_.size());
        for (const auto& [name, factory] : factories_) {
            (void)factory;
            names.push_back(name);
        }
        return names;
    }

private:
    std::unordered_map<std::string, Factory> factories_;
    std::unordered_map<std::type_index, std::string> typeNames_;
};

template<typename T> class ScriptRegistration final {
public:
    explicit ScriptRegistration(const char* name) { ScriptRegistry::instance().registerClass<T>(name); }
};
#define ENGINE_SCRIPT_JOIN_IMPL(a, b) a##b
#define ENGINE_SCRIPT_JOIN(a, b) ENGINE_SCRIPT_JOIN_IMPL(a, b)
#define ENGINE_REGISTER_SCRIPT(Type) static ::Engine::ScriptRegistration<Type> ENGINE_SCRIPT_JOIN(scriptRegistration_, __LINE__){#Type}

} // namespace Engine
