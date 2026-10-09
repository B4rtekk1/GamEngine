#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::wstring modulePath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (count == 0) throw std::runtime_error("Cannot locate launcher");
        if (count < buffer.size() - 1) return {buffer.data(), count};
        buffer.resize(buffer.size() * 2);
    }
}

std::string readCurrent(const std::filesystem::path& file) {
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open current.json");
    char buffer[4096]{};
    DWORD read = 0;
    const BOOL success = ReadFile(handle, buffer, sizeof(buffer) - 1, &read, nullptr);
    CloseHandle(handle);
    if (!success || read == sizeof(buffer) - 1) throw std::runtime_error("Invalid current.json");
    return {buffer, read};
}

std::wstring versionFromJson(const std::string& json) {
    auto key = json.find("\"Version\"");
    if (key == std::string::npos) throw std::runtime_error("Missing active version");
    auto colon = json.find(':', key + 9);
    auto begin = json.find('"', colon);
    auto end = json.find('"', begin + 1);
    if (colon == std::string::npos || begin == std::string::npos || end == std::string::npos)
        throw std::runtime_error("Invalid active version");
    const std::string value = json.substr(begin + 1, end - begin - 1);
    static const std::regex pattern(R"(^[0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z0-9.-]+)?$)");
    if (value.size() > 80 || !std::regex_match(value, pattern))
        throw std::runtime_error("Invalid active version");
    return {value.begin(), value.end()};
}

std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') {
            result.append(slashes * 2 + 1, L'\\');
            result += L'"';
        } else {
            result.append(slashes, L'\\');
            result += c;
        }
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result += L'"';
    return result;
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        const auto root = std::filesystem::path(modulePath()).parent_path();
        const auto version = versionFromJson(readCurrent(root / L"current.json"));
        const auto directory = root / L"Versions" / version;
        const auto editor = directory / L"Editor.exe";
        if (!std::filesystem::is_regular_file(editor)) throw std::runtime_error("Editor.exe not found");
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv == nullptr) throw std::runtime_error("Cannot read command line");
        std::wstring parameters;
        for (int i = 1; i < argc; ++i) {
            if (!parameters.empty()) parameters += L' ';
            parameters += quote(argv[i]);
        }
        LocalFree(argv);
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", editor.c_str(),
            parameters.empty() ? nullptr : parameters.c_str(), directory.c_str(), SW_SHOWNORMAL));
        if (result <= 32) throw std::runtime_error("Cannot start Editor.exe");
        return 0;
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "GamEngine", MB_OK | MB_ICONERROR);
        return 1;
    }
}
