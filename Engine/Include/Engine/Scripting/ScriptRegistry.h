#pragma once

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
    class Script;

    /** Registry of C++ script classes available to ScriptComponent. */
    class ScriptRegistry final {
    public:
        using Factory = std::function<std::unique_ptr<Script>()>;

        [[nodiscard]] static ScriptRegistry &instance() {
            static ScriptRegistry registry;
            return registry;
        }

        template<typename T>
        void registerClass(std::string name, std::string sourceFile = {}) {
            static_assert(std::derived_from<T, Script>);
            const std::string className = name;
            factories_.insert_or_assign(className, [] { return std::make_unique<T>(); });
            sourceFiles_.insert_or_assign(className, std::move(sourceFile));
            typeNames_.insert_or_assign(std::type_index(typeid(T)), std::move(name));
        }

        template<typename T>
        [[nodiscard]] std::optional<std::string> className() const {
            static_assert(std::derived_from<T, Script>);
            const auto found = typeNames_.find(std::type_index(typeid(T)));
            if (found == typeNames_.end()) { return std::nullopt;
}
            return found->second;
        }

        [[nodiscard]] std::unique_ptr<Script> create(std::string_view name) const;

        [[nodiscard]] std::optional<std::string> sourceFile(std::string_view name) const;

        [[nodiscard]] std::vector<std::string> classNames() const {
            std::vector<std::string> names;
            names.reserve(factories_.size());
            for (const auto &[name, factory]: factories_) {
                (void) factory;
                names.push_back(name);
            }
            return names;
        }

    private:
        std::unordered_map<std::string, Factory> factories_;
        std::unordered_map<std::string, std::string> sourceFiles_;
        std::unordered_map<std::type_index, std::string> typeNames_;
    };

    template<typename T>
    class ScriptRegistration final {
    public:
        explicit ScriptRegistration(const char *name, const char *sourceFile) {
            ScriptRegistry::instance().registerClass<T>(name, sourceFile);
        }
    };

#define ENGINE_SCRIPT_JOIN_IMPL(a, b) a##b
#define ENGINE_SCRIPT_JOIN(a, b) ENGINE_SCRIPT_JOIN_IMPL(a, b)
#define ENGINE_REGISTER_SCRIPT(Type) static ::Engine::ScriptRegistration<Type> ENGINE_SCRIPT_JOIN(scriptRegistration_, __LINE__){#Type, __FILE__}
} // namespace Engine
