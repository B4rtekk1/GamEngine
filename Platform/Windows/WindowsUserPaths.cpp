#include "Platform/UserPaths.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj_core.h>

#include <stdexcept>

namespace Platform {

std::filesystem::path UserPaths::localData() {
    PWSTR value = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &value);
    if (FAILED(result) || value == nullptr) throw std::runtime_error("Could not determine LocalAppData directory");
    const std::filesystem::path path{value};
    CoTaskMemFree(value);
    return path;
}

std::filesystem::path UserPaths::editorData() { return localData() / "GamEngine" / "Editor"; }
std::filesystem::path UserPaths::editorLogs() { return editorData() / "Logs"; }
std::filesystem::path UserPaths::editorCrashes() { return editorData() / "Crashes"; }
std::filesystem::path UserPaths::editorState() { return editorData() / "State"; }

} // namespace Platform
