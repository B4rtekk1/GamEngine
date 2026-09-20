#pragma once

#include "Engine/Scripting/DynamicLibrary.h"
#include "Engine/Scripting/ScriptRegistry.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace Engine {
    class Registry;
    class Scene;
    class ScriptRegistry;

    enum class ScriptModuleLoadMode {
        /** Load the supplied DLL directly; suitable for read-only game installs. */
        Direct,
        /** Load a versioned copy so the source DLL can be rebuilt on Windows. */
        HotReload,
    };

    /** Loads game-script DLLs and swaps them only after validation. */
    class ScriptModuleManager final {
    public:
        explicit ScriptModuleManager(ScriptRegistry &registry) noexcept : registry_(registry) {}
        ~ScriptModuleManager();
        ScriptModuleManager(const ScriptModuleManager &) = delete;
        ScriptModuleManager &operator=(const ScriptModuleManager &) = delete;

        [[nodiscard]] bool loadInitialModule(const std::filesystem::path &path,
                                             ScriptModuleLoadMode mode = ScriptModuleLoadMode::Direct);
        [[nodiscard]] bool tryReload(const std::filesystem::path &candidate, Registry &scene);
        [[nodiscard]] bool tryReload(const std::filesystem::path &candidate, Scene &scene);
        void unload(Registry &scene);
        void unload(Scene &scene);
        [[nodiscard]] std::uint64_t activeGeneration() const noexcept;

    private:
        struct LoadedModule {
            DynamicLibrary library;
            std::uint64_t generation = 0;
            std::filesystem::path path;
            std::vector<ScriptClassDescriptor> descriptors;
        };

        [[nodiscard]] bool loadCandidate(const std::filesystem::path &path, LoadedModule &out,
                                         ScriptModuleLoadMode mode);
        [[nodiscard]] static std::filesystem::path makeVersionedCopy(const std::filesystem::path &path,
                                                                     std::uint64_t generation);
        void destroyGeneration(Registry &scene, std::uint64_t generation) const;

        ScriptRegistry &registry_;
        LoadedModule active_;
        std::uint64_t nextGeneration_ = 1;
    };
} // namespace Engine
