#pragma once

#include <concepts>
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <cstdint>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace Engine {
    class Script;

    struct ScriptClassDescriptor {
        using Create = Script *(*)();
        using Destroy = void (*)(Script *);

        std::string name;
        Create create = nullptr;
        Destroy destroy = nullptr;
        std::string sourceFile;
        std::uint64_t moduleGeneration = 0;
    };

    struct RuntimeScriptInstance {
        Script *instance = nullptr;
        ScriptClassDescriptor::Destroy destroy = nullptr;
        std::uint64_t moduleGeneration = 0;

        RuntimeScriptInstance() = default;
        RuntimeScriptInstance(const RuntimeScriptInstance &) = delete;
        RuntimeScriptInstance &operator=(const RuntimeScriptInstance &) = delete;
        RuntimeScriptInstance(RuntimeScriptInstance &&other) noexcept;
        RuntimeScriptInstance &operator=(RuntimeScriptInstance &&other) noexcept;
        ~RuntimeScriptInstance() { reset(); }

        explicit operator bool() const noexcept { return instance != nullptr; }
        Script *operator->() const noexcept { return instance; }
        void reset() noexcept;
    };

    /** Passed through the C module entry point; registrations remain owned by Engine. */
    class ScriptModuleRegistrar final {
    public:
        explicit ScriptModuleRegistrar(class ScriptRegistry &registry) noexcept : registry_(registry) {}
        [[nodiscard]] std::uint64_t generation() const noexcept;
    private:
        class ScriptRegistry &registry_;
    };

    /** Registry of C++ script classes available to ScriptComponent. */
    class ScriptRegistry final {
    public:
        /** Process-wide registry, defined in Engine.dll (never inline in a game DLL). */
        [[nodiscard]] static ScriptRegistry &instance();

        template<typename T>
        void registerClass(std::string name, std::string sourceFile = {}) {
            static_assert(std::derived_from<T, Script>);
            const std::string className = name;
            registerClass(ScriptClassDescriptor{
                .name = std::move(name),
                .create = []() -> Script * { return new T(); },
                .destroy = [](Script *script) { delete static_cast<T *>(script); },
                .sourceFile = std::move(sourceFile),
                .moduleGeneration = activeRegistrationGeneration_});
            typeNames_.insert_or_assign(std::type_index(typeid(T)), className);
        }

        void registerClass(ScriptClassDescriptor descriptor);

        template<typename T>
        [[nodiscard]] std::optional<std::string> className() const {
            static_assert(std::derived_from<T, Script>);
            const auto found = typeNames_.find(std::type_index(typeid(T)));
            if (found == typeNames_.end()) { return std::nullopt;
}
            return found->second;
        }

        [[nodiscard]] RuntimeScriptInstance create(std::string_view name) const;

        [[nodiscard]] std::optional<std::string> sourceFile(std::string_view name) const;

        [[nodiscard]] std::vector<std::string> classNames() const {
            std::vector<std::string> names;
            names.reserve(classes_.size());
            for (const auto &[name, descriptor]: classes_) {
                (void) descriptor;
                names.push_back(name);
            }
            std::sort(names.begin(), names.end());
            return names;
        }

        void removeGeneration(std::uint64_t generation);
        void setActiveRegistrationGeneration(std::uint64_t generation) noexcept { activeRegistrationGeneration_ = generation; }
        [[nodiscard]] std::uint64_t activeRegistrationGeneration() const noexcept { return activeRegistrationGeneration_; }

    private:
        std::unordered_map<std::string, ScriptClassDescriptor> classes_;
        std::unordered_map<std::type_index, std::string> typeNames_;
        std::uint64_t activeRegistrationGeneration_ = 0;
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
