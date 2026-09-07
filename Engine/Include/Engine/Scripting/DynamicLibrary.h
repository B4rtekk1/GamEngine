#pragma once

#include <filesystem>

namespace Engine {
    /** RAII wrapper around one native shared library. */
    class DynamicLibrary final {
    public:
        DynamicLibrary() = default;
        ~DynamicLibrary();
        DynamicLibrary(const DynamicLibrary &) = delete;
        DynamicLibrary &operator=(const DynamicLibrary &) = delete;
        DynamicLibrary(DynamicLibrary &&other) noexcept;
        DynamicLibrary &operator=(DynamicLibrary &&other) noexcept;

        [[nodiscard]] bool load(const std::filesystem::path &path);
        void unload() noexcept;
        [[nodiscard]] bool loaded() const noexcept;
        [[nodiscard]] void *symbolRaw(const char *name) const noexcept;

        template<typename T>
        [[nodiscard]] T symbol(const char *name) const noexcept {
            return reinterpret_cast<T>(symbolRaw(name));
        }

    private:
#ifdef _WIN32
        void *handle_ = nullptr;
#else
        void *handle_ = nullptr;
#endif
    };
} // namespace Engine
