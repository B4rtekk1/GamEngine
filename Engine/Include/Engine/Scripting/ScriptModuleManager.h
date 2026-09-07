#pragma once

#include "Engine/Scripting/DynamicLibrary.h"

#include <cstdint>
#include <filesystem>

namespace Engine {
    class Registry;
    class Scene;
    class ScriptRegistry;

    /** Loads versioned game-script DLLs and swaps them only after validation. */
    class ScriptModuleManager final {
    public:
        explicit ScriptModuleManager(ScriptRegistry &registry) noexcept : registry_(registry) {}
        ~ScriptModuleManager();
        ScriptModuleManager(const ScriptModuleManager &) = delete;
        ScriptModuleManager &operator=(const ScriptModuleManager &) = delete;

        [[nodiscard]] bool loadInitialModule(const std::filesystem::path &path);
        [[nodiscard]] bool tryReload(const std::filesystem::path &candidate, Registry &scene);
        void unload(Registry &scene);
        void unload(Scene &scene);
        [[nodiscard]] std::uint64_t activeGeneration() const noexcept;

    private:
        struct LoadedModule {
            DynamicLibrary library;
            std::uint64_t generation = 0;
            std::filesystem::path path;
        };

        [[nodiscard]] bool loadCandidate(const std::filesystem::path &path, LoadedModule &out);
        void destroyGeneration(Registry &scene, std::uint64_t generation) const;

        ScriptRegistry &registry_;
        LoadedModule active_;
        std::uint64_t nextGeneration_ = 1;
    };
} // namespace Engine
