#include "ReleaseClient.h"

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <commctrl.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace {
constexpr UINT WM_SETUP_STATUS = WM_APP + 1;
constexpr UINT WM_SETUP_PROGRESS = WM_APP + 2;
constexpr UINT WM_SETUP_DONE = WM_APP + 3;
constexpr int ID_INSTALL = 101;
constexpr int ID_PATH = 102;
constexpr int ID_DESKTOP = 103;
constexpr int ID_START = 104;
constexpr wchar_t markerName[] = L"gamengine-install.json";
constexpr wchar_t registryKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\GamEngine";
HWND mainWindow{}, pathEdit{}, installButton{}, progressBar{}, statusLabel{}, desktopCheck{}, startCheck{};
std::atomic_bool installing = false;

std::wstring quote(const std::wstring& value) { return L'"' + value + L'"'; }

std::wstring modulePath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!count) throw std::runtime_error("Cannot locate setup executable");
        if (count < buffer.size() - 1) return {buffer.data(), count};
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring localAppData() {
    wchar_t buffer[32768]{};
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, 32768);
    if (!count || count >= 32768) throw std::runtime_error("LOCALAPPDATA is unavailable");
    return {buffer, count};
}

std::wstring specialFolder(const wchar_t* name) {
    wchar_t buffer[32768]{};
    const DWORD count = GetEnvironmentVariableW(name, buffer, 32768);
    if (!count || count >= 32768) throw std::runtime_error("A Windows user folder is unavailable");
    return {buffer, count};
}

fs::path knownFolder(REFKNOWNFOLDERID id) {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &value)))
        throw std::runtime_error("Cannot locate a Windows user folder");
    const fs::path result(value);
    CoTaskMemFree(value);
    return result;
}

std::wstring guid() {
    GUID value{};
    if (CoCreateGuid(&value) != S_OK) throw std::runtime_error("Cannot create temporary folder name");
    wchar_t text[40]{};
    StringFromGUID2(value, text, 40);
    std::wstring result(text);
    result.erase(0, 1);
    result.pop_back();
    return result;
}

std::string readText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read installation state");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string jsonString(const std::string& json, const std::string& key, bool optional = false) {
    auto pos = json.find('"' + key + '"');
    if (pos == std::string::npos) {
        if (optional) return {};
        throw std::runtime_error("Invalid installation state");
    }
    pos = json.find(':', pos + key.size() + 2);
    if (pos == std::string::npos) throw std::runtime_error("Invalid installation state");
    pos = json.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == std::string::npos) throw std::runtime_error("Invalid installation state");
    if (json.compare(pos, 4, "null") == 0 && optional) return {};
    if (json[pos] != '"') throw std::runtime_error("Invalid installation state");
    const auto end = json.find('"', pos + 1);
    if (end == std::string::npos) throw std::runtime_error("Invalid installation state");
    return json.substr(pos + 1, end - pos - 1);
}

bool validVersion(const std::string& version) {
    static const std::regex pattern(R"(^[0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z0-9.-]+)?$)");
    return version.size() <= 80 && std::regex_match(version, pattern);
}

void writeState(const fs::path& root, const std::string& version, const std::string& previous) {
    if (!validVersion(version) || (!previous.empty() && !validVersion(previous)))
        throw std::runtime_error("Invalid version state");
    const auto temporary = root / (L"current-" + guid() + L".tmp");
    const std::string json = "{\"Version\":\"" + version + "\",\"PreviousVersion\":" +
        (previous.empty() ? "null" : "\"" + previous + "\"") + "}";
    { std::ofstream output(temporary, std::ios::binary); output << json; if (!output) throw std::runtime_error("Cannot write version state"); }
    if (!MoveFileExW(temporary.c_str(), (root / L"current.json").c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fs::remove(temporary);
        throw std::runtime_error("Cannot activate new version");
    }
}

std::string currentVersion(const fs::path& root) {
    const auto path = root / L"current.json";
    if (!fs::exists(path)) return {};
    const auto version = jsonString(readText(path), "Version");
    if (!validVersion(version)) throw std::runtime_error("Invalid active version");
    return version;
}

void validateRoot(const fs::path& root) {
    const auto full = fs::absolute(root).lexically_normal();
    if (full == full.root_path() || full.empty()) throw std::runtime_error("Choose a dedicated installation folder");
    if (fs::exists(full) && !fs::is_directory(full)) throw std::runtime_error("Installation path is not a folder");
    if (fs::exists(full) && fs::directory_iterator(full) != fs::directory_iterator() && !fs::exists(full / markerName))
        throw std::runtime_error("Selected folder is not empty and is not a GamEngine installation");
}

void postStatus(const std::wstring& message) {
    PostMessageW(mainWindow, WM_SETUP_STATUS, 0, reinterpret_cast<LPARAM>(new std::wstring(message)));
}

void createShortcut(const fs::path& shortcut, const fs::path& setup, const fs::path& root) {
    fs::create_directories(shortcut.parent_path());
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
        throw std::runtime_error("Cannot create Windows shortcut");
    link->SetPath(setup.c_str());
    link->SetArguments(L"--launch");
    link->SetWorkingDirectory(root.c_str());
    IPersistFile* file = nullptr;
    const auto result = link->QueryInterface(IID_PPV_ARGS(&file));
    if (FAILED(result)) { link->Release(); throw std::runtime_error("Cannot save Windows shortcut"); }
    const auto saved = file->Save(shortcut.c_str(), TRUE);
    file->Release();
    link->Release();
    if (FAILED(saved)) throw std::runtime_error("Cannot save Windows shortcut");
}

void registerInstall(const fs::path& root, const std::string& version, bool desktop, bool startMenu) {
    const auto setup = root / L"GamEngineSetup.exe";
    const auto source = fs::path(modulePath());
    std::error_code compareError;
    if (!fs::equivalent(source, setup, compareError)) {
        const auto temporary = root / (L".setup-" + guid() + L".tmp");
        fs::copy_file(source, temporary);
        if (!MoveFileExW(temporary.c_str(), setup.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            fs::remove(temporary);
            throw std::runtime_error("Cannot update installed setup executable");
        }
    }
    const auto desktopLink = knownFolder(FOLDERID_Desktop) / L"GamEngine.lnk";
    const auto menuDir = knownFolder(FOLDERID_Programs) / L"GamEngine";
    const auto menuLink = menuDir / L"GamEngine.lnk";
    if (desktop) createShortcut(desktopLink, setup, root);
    else fs::remove(desktopLink);
    if (startMenu) createShortcut(menuLink, setup, root);
    else fs::remove(menuLink);
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, registryKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot register GamEngine in Installed apps");
    const auto set = [&](const wchar_t* name, const std::wstring& value) {
        if (RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS)
            throw std::runtime_error("Cannot write Installed apps entry");
    };
    try {
        set(L"DisplayName", L"GamEngine");
        set(L"DisplayVersion", std::wstring(version.begin(), version.end()));
        set(L"Publisher", L"GamEngine");
        set(L"InstallLocation", root.wstring());
        set(L"DisplayIcon", setup.wstring());
        set(L"UninstallString", quote(setup.wstring()) + L" --uninstall");
    } catch (...) { RegCloseKey(key); throw; }
    RegCloseKey(key);
}

void installTo(const fs::path& requestedRoot, bool desktop, bool startMenu, bool registerApplication = true) {
    const auto root = fs::absolute(requestedRoot).lexically_normal();
    validateRoot(root);
    postStatus(L"Checking the latest GitHub release...");
    const auto asset = latestEditorRelease();
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(root.root_path().c_str(), &available, nullptr, nullptr) ||
        available.QuadPart < asset.size * 6)
        throw std::runtime_error("Not enough disk space to install GamEngine");
    const auto oldVersion = currentVersion(root);
    const auto target = root / L"Versions" / std::wstring(asset.version.begin(), asset.version.end());
    const auto backup = root / L"Versions" / (L".backup-" + guid());
    const auto stage = root / (L".staging-" + guid());
    const auto archive = fs::path(specialFolder(L"TEMP")) / (L"GamEngine-" + guid() + L".zip");
    bool replaced = false, activated = false;
    try {
        postStatus(L"Downloading GamEngineEditor.zip...");
        downloadEditor(asset, archive, [](int percent) { PostMessageW(mainWindow, WM_SETUP_PROGRESS, percent, 0); });
        postStatus(L"Validating the release archive...");
        validateArchive(archive);
        PostMessageW(mainWindow, WM_SETUP_PROGRESS, 75, 0);
        fs::create_directories(stage);
        postStatus(L"Extracting GamEngine...");
        extractArchive(archive, stage);
        const auto editorRoot = fs::exists(stage / L"GamEngineEditor" / L"Editor.exe") ? stage / L"GamEngineEditor" : stage;
        for (const auto& required : {L"Editor.exe", L"Engine.dll", L"DearImGui.dll", L"SDL3.dll", L"Tools/Slang/slangc.exe"})
            if (!fs::is_regular_file(editorRoot / required)) throw std::runtime_error("Release archive is missing required files");
        fs::create_directories(target.parent_path());
        if (fs::exists(target)) { fs::rename(target, backup); replaced = true; }
        fs::rename(editorRoot, target);
        activated = true;
        if (fs::exists(stage)) fs::remove_all(stage);
        writeState(root, asset.version, oldVersion == asset.version ? "" : oldVersion);
        { std::ofstream marker(root / markerName, std::ios::binary); marker << "GamEngine"; }
        if (registerApplication) {
            const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            try { registerInstall(root, asset.version, desktop, startMenu); }
            catch (...) { if (SUCCEEDED(apartment)) CoUninitialize(); throw; }
            if (SUCCEEDED(apartment)) CoUninitialize();
        }
        if (replaced) fs::remove_all(backup);
        PostMessageW(mainWindow, WM_SETUP_PROGRESS, 100, 0);
        postStatus(L"GamEngine is ready.");
    } catch (...) {
        if (fs::exists(stage)) fs::remove_all(stage);
        if (activated && fs::exists(target)) fs::remove_all(target);
        if (replaced && fs::exists(backup)) fs::rename(backup, target);
        if (!oldVersion.empty() && fs::exists(root / L"Versions" / std::wstring(oldVersion.begin(), oldVersion.end())))
            writeState(root, oldVersion, "");
        else { fs::remove(root / L"current.json"); fs::remove(root / markerName); }
        fs::remove(archive);
        throw;
    }
    fs::remove(archive);
}

void launchEditor(const fs::path& root) {
    const auto version = currentVersion(root);
    if (version.empty()) throw std::runtime_error("No active GamEngine version");
    const auto directory = root / L"Versions" / std::wstring(version.begin(), version.end());
    const auto editor = directory / L"Editor.exe";
    if (!fs::is_regular_file(editor)) throw std::runtime_error("Editor.exe is missing");
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", editor.c_str(), nullptr, directory.c_str(), SW_SHOWNORMAL)) <= 32)
        throw std::runtime_error("Cannot start Editor.exe");
}

void rollback(const fs::path& root) {
    const auto state = readText(root / L"current.json");
    const auto version = jsonString(state, "Version");
    const auto previous = jsonString(state, "PreviousVersion", true);
    if (!validVersion(previous) || !fs::is_regular_file(root / L"Versions" / std::wstring(previous.begin(), previous.end()) / L"Editor.exe"))
        throw std::runtime_error("No previous version is available");
    writeState(root, previous, version);
}

void uninstallWorker(const fs::path& root, DWORD parent, bool deleteSelf = true) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, parent);
    if (process) { WaitForSingleObject(process, 30000); CloseHandle(process); }
    if (!fs::is_regular_file(root / markerName) || readText(root / markerName) != "GamEngine")
        throw std::runtime_error("GamEngine installation marker is missing or invalid");
    if (fs::exists(root / L"Versions")) fs::remove_all(root / L"Versions");
    for (const auto& name : {L"current.json", markerName, L"GamEngineSetup.exe", L"GamEngineLauncher.exe"}) fs::remove(root / name);
    const auto desktopLink = knownFolder(FOLDERID_Desktop) / L"GamEngine.lnk";
    const auto menuDir = knownFolder(FOLDERID_Programs) / L"GamEngine";
    fs::remove(desktopLink);
    fs::remove(menuDir / L"GamEngine.lnk");
    if (fs::exists(menuDir) && fs::is_empty(menuDir)) fs::remove(menuDir);
    RegDeleteTreeW(HKEY_CURRENT_USER, registryKey);
    if (fs::exists(root) && fs::is_empty(root)) fs::remove(root);
    if (deleteSelf) MoveFileExW(modulePath().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
}

void startUninstall(const fs::path& root) {
    if (!fs::exists(root / markerName)) throw std::runtime_error("GamEngine installation marker is missing");
    const auto temporary = fs::path(specialFolder(L"TEMP")) / (L"GamEngineUninstall-" + guid() + L".exe");
    fs::copy_file(modulePath(), temporary);
    std::wstring command = quote(temporary.wstring()) + L" --uninstall-worker " + quote(root.wstring()) + L" " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(temporary.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) throw std::runtime_error("Cannot start uninstaller");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

std::wstring editText(HWND edit) {
    const int count = GetWindowTextLengthW(edit);
    std::wstring value(count + 1, L'\0');
    GetWindowTextW(edit, value.data(), count + 1);
    value.resize(count);
    return value;
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        CreateWindowW(L"STATIC", L"Install GamEngine", WS_CHILD | WS_VISIBLE, 24, 20, 550, 30, window, nullptr, nullptr, nullptr);
        CreateWindowW(L"STATIC", L"The latest release will be downloaded from GitHub.", WS_CHILD | WS_VISIBLE, 24, 61, 550, 22, window, nullptr, nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Installation folder", WS_CHILD | WS_VISIBLE, 24, 101, 550, 22, window, nullptr, nullptr, nullptr);
        pathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", (fs::path(localAppData()) / L"Programs" / L"GamEngine").c_str(),
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 24, 126, 560, 26, window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_PATH)), nullptr, nullptr);
        desktopCheck = CreateWindowW(L"BUTTON", L"Create desktop shortcut", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                      24, 174, 300, 26, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_DESKTOP)), nullptr, nullptr);
        startCheck = CreateWindowW(L"BUTTON", L"Add to Start menu", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                    24, 207, 300, 26, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_START)), nullptr, nullptr);
        SendMessageW(desktopCheck, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(startCheck, BM_SETCHECK, BST_CHECKED, 0);
        progressBar = CreateWindowW(PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE, 24, 253, 560, 20, window, nullptr, nullptr, nullptr);
        SendMessageW(progressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        statusLabel = CreateWindowW(L"STATIC", L"Ready to install", WS_CHILD | WS_VISIBLE, 24, 287, 560, 25, window, nullptr, nullptr, nullptr);
        installButton = CreateWindowW(L"BUTTON", L"Install", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                      469, 326, 115, 32, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_INSTALL)), nullptr, nullptr);
        const auto font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        EnumChildWindows(window, [](HWND child, LPARAM param) -> BOOL {
            SendMessageW(child, WM_SETFONT, param, TRUE); return TRUE;
        }, reinterpret_cast<LPARAM>(font));
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == ID_INSTALL && !installing.exchange(true)) {
            const auto root = editText(pathEdit);
            const bool desktop = SendMessageW(desktopCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            const bool start = SendMessageW(startCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            EnableWindow(installButton, FALSE);
            EnableWindow(pathEdit, FALSE);
            std::thread([root, desktop, start] {
                try { installTo(root, desktop, start); PostMessageW(mainWindow, WM_SETUP_DONE, 1, 0); }
                catch (const std::exception& error) {
                    std::wstring message(error.what(), error.what() + std::strlen(error.what()));
                    PostMessageW(mainWindow, WM_SETUP_DONE, 0, reinterpret_cast<LPARAM>(new std::wstring(message)));
                }
            }).detach();
        }
        return 0;
    case WM_SETUP_STATUS:
        SetWindowTextW(statusLabel, reinterpret_cast<std::wstring*>(lparam)->c_str());
        delete reinterpret_cast<std::wstring*>(lparam);
        return 0;
    case WM_SETUP_PROGRESS:
        SendMessageW(progressBar, PBM_SETPOS, wparam, 0);
        return 0;
    case WM_SETUP_DONE:
        installing = false;
        if (wparam) SetWindowTextW(installButton, L"Done");
        else {
            const auto error = reinterpret_cast<std::wstring*>(lparam);
            SetWindowTextW(statusLabel, (L"Error: " + *error).c_str());
            delete error;
            EnableWindow(installButton, TRUE);
            EnableWindow(pathEdit, TRUE);
        }
        return 0;
    case WM_CLOSE:
        if (!installing) DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    try {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv) throw std::runtime_error("Cannot read command line");
        const auto root = fs::path(modulePath()).parent_path();
        const std::wstring command = argc > 1 ? argv[1] : L"";
        if (command == L"--launch") { LocalFree(argv); launchEditor(root); return 0; }
        if (command == L"--rollback") { LocalFree(argv); rollback(root); return 0; }
        if (command == L"--verify-release" && argc == 3) {
            const auto destination = fs::path(argv[2]);
            LocalFree(argv);
            const auto asset = latestEditorRelease();
            downloadEditor(asset, destination, [](int) {});
            validateArchive(destination);
            return 0;
        }
#ifdef GE_SETUP_TEST_COMMANDS
        if (command == L"--test-install" && argc == 3) {
            const auto destination = fs::path(argv[2]);
            LocalFree(argv);
            installTo(destination, false, false, false);
            return 0;
        }
        if (command == L"--test-full-install" && argc == 3) {
            const auto destination = fs::path(argv[2]);
            LocalFree(argv);
            installTo(destination, true, true);
            return 0;
        }
        if (command == L"--test-uninstall" && argc == 3) {
            const auto destination = fs::path(argv[2]);
            LocalFree(argv);
            uninstallWorker(destination, 0, false);
            return 0;
        }
#endif
        if (command == L"--uninstall-worker" && argc == 4) {
            const auto installedRoot = fs::path(argv[2]);
            const DWORD parent = std::stoul(std::wstring(argv[3]));
            LocalFree(argv);
            uninstallWorker(installedRoot, parent);
            return 0;
        }
        if (command == L"--uninstall") {
            LocalFree(argv);
            if (MessageBoxW(nullptr, L"Uninstall GamEngine and all installed versions?", L"GamEngine",
                            MB_YESNO | MB_ICONQUESTION) == IDYES) startUninstall(root);
            return 0;
        }
        LocalFree(argv);
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
        InitCommonControlsEx(&controls);
        WNDCLASSEXW type{sizeof(type)};
        type.lpfnWndProc = windowProc;
        type.hInstance = instance;
        type.lpszClassName = L"GamEngineSetupWindow";
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassExW(&type);
        mainWindow = CreateWindowExW(0, type.lpszClassName, L"GamEngine Setup",
                                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                    CW_USEDEFAULT, CW_USEDEFAULT, 626, 415, nullptr, nullptr, instance, nullptr);
        if (!mainWindow) throw std::runtime_error("Cannot create setup window");
        ShowWindow(mainWindow, show);
        UpdateWindow(mainWindow);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
        return static_cast<int>(message.wParam);
    } catch (const std::exception& error) {
        const std::wstring command = GetCommandLineW();
        if (command.find(L"--verify-release") != std::wstring::npos ||
#ifdef GE_SETUP_TEST_COMMANDS
            command.find(L"--test-install") != std::wstring::npos ||
            command.find(L"--test-full-install") != std::wstring::npos ||
            command.find(L"--test-uninstall") != std::wstring::npos ||
#endif
            command.find(L"--uninstall-worker") != std::wstring::npos) {
            try {
                std::ofstream log(fs::path(specialFolder(L"TEMP")) / L"GamEngineSetupError.log", std::ios::binary);
                log << error.what();
            } catch (...) {}
            return 1;
        }
        MessageBoxA(nullptr, error.what(), "GamEngine Setup", MB_OK | MB_ICONERROR);
        return 1;
    }
}
