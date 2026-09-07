#include "Engine/Scripting/DynamicLibrary.h"

#ifdef _WIN32
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

#include <utility>

namespace Engine {
    DynamicLibrary::~DynamicLibrary() { unload(); }

    DynamicLibrary::DynamicLibrary(DynamicLibrary &&other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    DynamicLibrary &DynamicLibrary::operator=(DynamicLibrary &&other) noexcept {
        if (this != &other) {
            unload();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    bool DynamicLibrary::load(const std::filesystem::path &path) {
        unload();
#ifdef _WIN32
        handle_ = reinterpret_cast<void *>(LoadLibraryW(path.c_str()));
#else
        handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        return handle_ != nullptr;
    }

    void DynamicLibrary::unload() noexcept {
        if (handle_ == nullptr) return;
#ifdef _WIN32
        FreeLibrary(reinterpret_cast<HMODULE>(handle_));
#else
        dlclose(handle_);
#endif
        handle_ = nullptr;
    }

    bool DynamicLibrary::loaded() const noexcept { return handle_ != nullptr; }

    void *DynamicLibrary::symbolRaw(const char *name) const noexcept {
        if (handle_ == nullptr) return nullptr;
#ifdef _WIN32
        return reinterpret_cast<void *>(GetProcAddress(reinterpret_cast<HMODULE>(handle_), name));
#else
        return dlsym(handle_, name);
#endif
    }
} // namespace Engine
