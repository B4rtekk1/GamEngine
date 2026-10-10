#include "ReleaseClient.h"

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <commctrl.h>
#include <richedit.h>
#include <dwmapi.h>
#include <gdiplus.h>

#include <algorithm>
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
    constexpr int ID_BROWSE = 105;
    constexpr int ID_CONSENT = 106;
    constexpr int ID_PROJECT = 107;
    constexpr int ID_LICENSE_TEXT = 109;
    constexpr int ID_BACK = 110;
    constexpr int ID_RUN = 111;
    constexpr wchar_t markerName[] = L"gamengine-install.json";
    constexpr wchar_t registryKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\GamEngine";
    HWND mainWindow{}, pathEdit{}, installButton{}, backButton{}, statusLabel{}, desktopCheck{}, startCheck{},
            consentCheck{}, browseButton{}, licenseText{}, projectButton{}, runCheck{};
    HFONT bodyFont{}, smallFont{}, buttonFont{}, linkFont{};
    HBRUSH whiteBrush{}, cardBrush{};
    int progressValue = 0;
    bool installationFailed = false;
    bool desktopSelected = true;
    bool startSelected = true;
    bool consentSelected = false;
    bool licenseAvailable = false;
    bool runAfterInstall = true;
    bool projectHovered = false;
    int licenseWheelRemainder = 0;
    std::atomic_bool installing = false;
    UINT uiDpi = 96;
    bool logicalPainting = false;

    int px(int value) { return MulDiv(value, static_cast<int>(uiDpi), 96); }

    void refreshFonts(HWND window);

    RECT paintRect(RECT area) {
        if (!logicalPainting) return area;
        return {px(area.left), px(area.top), px(area.right), px(area.bottom)};
    }

    enum class SetupStep { welcome, preferences, license, progress, complete };

    SetupStep currentStep = SetupStep::welcome;

    std::wstring quote(const std::wstring &value) { return L'"' + value + L'"'; }

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

    std::wstring specialFolder(const wchar_t *name) {
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

    std::string readText(const fs::path &path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("Cannot read installation state");
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    std::string jsonString(const std::string &json, const std::string &key, bool optional = false) {
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

    bool validVersion(const std::string &version) {
        static const std::regex pattern(R"(^[0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z0-9.-]+)?$)");
        return version.size() <= 80 && std::regex_match(version, pattern);
    }

    void writeState(const fs::path &root, const std::string &version, const std::string &previous) {
        if (!validVersion(version) || (!previous.empty() && !validVersion(previous)))
            throw std::runtime_error("Invalid version state");
        const auto temporary = root / (L"current-" + guid() + L".tmp");
        const std::string json = "{\"Version\":\"" + version + "\",\"PreviousVersion\":" +
                                 (previous.empty() ? "null" : "\"" + previous + "\"") + "}";
        {
            std::ofstream output(temporary, std::ios::binary);
            output << json;
            if (!output) throw std::runtime_error("Cannot write version state");
        }
        if (!MoveFileExW(temporary.c_str(), (root / L"current.json").c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            fs::remove(temporary);
            throw std::runtime_error("Cannot activate new version");
        }
    }

    std::string currentVersion(const fs::path &root) {
        const auto path = root / L"current.json";
        if (!fs::exists(path)) return {};
        const auto version = jsonString(readText(path), "Version");
        if (!validVersion(version)) throw std::runtime_error("Invalid active version");
        return version;
    }

    void validateRoot(const fs::path &root) {
        const auto full = fs::absolute(root).lexically_normal();
        if (full == full.root_path() || full.empty())
            throw
                    std::runtime_error("Choose a dedicated installation folder");
        if (fs::exists(full) && !fs::is_directory(full)) throw std::runtime_error("Installation path is not a folder");
        if (fs::exists(full) && fs::directory_iterator(full) != fs::directory_iterator() && !fs::exists(
                full / markerName))
            throw std::runtime_error("Selected folder is not empty and is not a GamEngine installation");
    }

    void postStatus(const std::wstring &message) {
        PostMessageW(mainWindow, WM_SETUP_STATUS, 0, reinterpret_cast<LPARAM>(new std::wstring(message)));
    }

    void createShortcut(const fs::path &shortcut, const fs::path &setup, const fs::path &root) {
        fs::create_directories(shortcut.parent_path());
        IShellLinkW *link = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
            throw std::runtime_error("Cannot create Windows shortcut");
        link->SetPath(setup.c_str());
        link->SetArguments(L"--launch");
        link->SetWorkingDirectory(root.c_str());
        IPersistFile *file = nullptr;
        const auto result = link->QueryInterface(IID_PPV_ARGS(&file));
        if (FAILED(result)) {
            link->Release();
            throw std::runtime_error("Cannot save Windows shortcut");
        }
        const auto saved = file->Save(shortcut.c_str(), TRUE);
        file->Release();
        link->Release();
        if (FAILED(saved)) throw std::runtime_error("Cannot save Windows shortcut");
    }

    void registerInstall(const fs::path &root, const std::string &version, bool desktop, bool startMenu) {
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
        if (RegCreateKeyExW(HKEY_CURRENT_USER, registryKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
            ERROR_SUCCESS)
            throw std::runtime_error("Cannot register GamEngine in Installed apps");
        const auto set = [&](const wchar_t *name, const std::wstring &value) {
            if (RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
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
        } catch (...) {
            RegCloseKey(key);
            throw;
        }
        RegCloseKey(key);
    }

    void installTo(const fs::path &requestedRoot, bool desktop, bool startMenu, bool registerApplication = true) {
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
            downloadEditor(asset, archive, [](int percent) {
                PostMessageW(mainWindow, WM_SETUP_PROGRESS, percent, 0);
            });
            postStatus(L"Validating the release archive...");
            validateArchive(archive);
            PostMessageW(mainWindow, WM_SETUP_PROGRESS, 75, 0);
            fs::create_directories(stage);
            postStatus(L"Extracting GamEngine...");
            extractArchive(archive, stage);
            const auto editorRoot = fs::exists(stage / L"GamEngineEditor" / L"Editor.exe")
                                        ? stage / L"GamEngineEditor"
                                        : stage;
            for (const auto &required: {
                     L"Editor.exe", L"Engine.dll", L"DearImGui.dll", L"SDL3.dll", L"Tools/Slang/slangc.exe"
                 })
                if (!fs::is_regular_file(editorRoot / required))
                    throw std::runtime_error(
                        "Release archive is missing required files");
            fs::create_directories(target.parent_path());
            if (fs::exists(target)) {
                fs::rename(target, backup);
                replaced = true;
            }
            fs::rename(editorRoot, target);
            activated = true;
            if (fs::exists(stage)) fs::remove_all(stage);
            writeState(root, asset.version, oldVersion == asset.version ? "" : oldVersion);
            {
                std::ofstream marker(root / markerName, std::ios::binary);
                marker << "GamEngine";
            }
            if (registerApplication) {
                const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                try { registerInstall(root, asset.version, desktop, startMenu); } catch (...) {
                    if (SUCCEEDED(apartment)) CoUninitialize();
                    throw;
                }
                if (SUCCEEDED(apartment)) CoUninitialize();
            }
            if (replaced) fs::remove_all(backup);
            PostMessageW(mainWindow, WM_SETUP_PROGRESS, 100, 0);
            postStatus(L"GamEngine is ready.");
        } catch (...) {
            if (fs::exists(stage)) fs::remove_all(stage);
            if (activated && fs::exists(target)) fs::remove_all(target);
            if (replaced && fs::exists(backup)) fs::rename(backup, target);
            if (!oldVersion.empty() && fs::exists(
                    root / L"Versions" / std::wstring(oldVersion.begin(), oldVersion.end())))
                writeState(root, oldVersion, "");
            else {
                fs::remove(root / L"current.json");
                fs::remove(root / markerName);
            }
            fs::remove(archive);
            throw;
        }
        fs::remove(archive);
    }

    void launchEditor(const fs::path &root) {
        const auto version = currentVersion(root);
        if (version.empty()) throw std::runtime_error("No active GamEngine version");
        const auto directory = root / L"Versions" / std::wstring(version.begin(), version.end());
        const auto editor = directory / L"Editor.exe";
        if (!fs::is_regular_file(editor)) throw std::runtime_error("Editor.exe is missing");
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", editor.c_str(), nullptr, directory.c_str(),
                                                    SW_SHOWNORMAL)) <= 32)
            throw std::runtime_error("Cannot start Editor.exe");
    }

    void rollback(const fs::path &root) {
        const auto state = readText(root / L"current.json");
        const auto version = jsonString(state, "Version");
        const auto previous = jsonString(state, "PreviousVersion", true);
        if (!validVersion(previous) || !fs::is_regular_file(
                root / L"Versions" / std::wstring(previous.begin(), previous.end()) / L"Editor.exe"))
            throw std::runtime_error("No previous version is available");
        writeState(root, previous, version);
    }

    void uninstallWorker(const fs::path &root, DWORD parent, bool deleteSelf = true) {
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, parent);
        if (process) {
            WaitForSingleObject(process, 30000);
            CloseHandle(process);
        }
        if (!fs::is_regular_file(root / markerName) || readText(root / markerName) != "GamEngine")
            throw std::runtime_error("GamEngine installation marker is missing or invalid");
        if (fs::exists(root / L"Versions")) fs::remove_all(root / L"Versions");
        for (const auto &name: {
                 L"current.json", markerName, L"GamEngineSetup.exe", L"GamEngineLauncher.exe"
             })
            fs::remove(root / name);
        const auto desktopLink = knownFolder(FOLDERID_Desktop) / L"GamEngine.lnk";
        const auto menuDir = knownFolder(FOLDERID_Programs) / L"GamEngine";
        fs::remove(desktopLink);
        fs::remove(menuDir / L"GamEngine.lnk");
        if (fs::exists(menuDir) && fs::is_empty(menuDir)) fs::remove(menuDir);
        RegDeleteTreeW(HKEY_CURRENT_USER, registryKey);
        if (fs::exists(root) && fs::is_empty(root)) fs::remove(root);
        if (deleteSelf) MoveFileExW(modulePath().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }

    void startUninstall(const fs::path &root) {
        if (!fs::exists(root / markerName)) throw std::runtime_error("GamEngine installation marker is missing");
        const auto temporary = fs::path(specialFolder(L"TEMP")) / (L"GamEngineUninstall-" + guid() + L".exe");
        fs::copy_file(modulePath(), temporary);
        std::wstring command = quote(temporary.wstring()) + L" --uninstall-worker " + quote(root.wstring()) + L" " +
                               std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(temporary.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &startup, &process))
            throw std::runtime_error("Cannot start uninstaller");
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

    void drawText(HDC dc, const wchar_t *value, RECT area, HFONT font, COLORREF color,
                  UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX) {
        area = paintRect(area);
        const auto oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, color);
        DrawTextW(dc, value, -1, &area, format);
        SelectObject(dc, oldFont);
    }

    void roundedFill(HDC dc, RECT area, COLORREF color, int radius) {
        if (logicalPainting) radius = px(radius);
        area = paintRect(area);
        struct GdiPlusRuntime {
            ULONG_PTR token{};

            GdiPlusRuntime() {
                Gdiplus::GdiplusStartupInput input;
                Gdiplus::GdiplusStartup(&token, &input, nullptr);
            }

            ~GdiPlusRuntime() { if (token) Gdiplus::GdiplusShutdown(token); }
        };
        static GdiPlusRuntime runtime;
        if (!runtime.token) return;
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const float left = static_cast<float>(area.left);
        const float top = static_cast<float>(area.top);
        const float width = static_cast<float>(area.right - area.left);
        const float height = static_cast<float>(area.bottom - area.top);
        const float diameter = static_cast<float>(std::min<LONG>(radius * 2,
                                                                 std::min(area.right - area.left,
                                                                          area.bottom - area.top)));
        Gdiplus::GraphicsPath path;
        path.AddArc(left, top, diameter, diameter, 180, 90);
        path.AddArc(left + width - diameter, top, diameter, diameter, 270, 90);
        path.AddArc(left + width - diameter, top + height - diameter, diameter, diameter, 0, 90);
        path.AddArc(left, top + height - diameter, diameter, diameter, 90, 90);
        path.CloseFigure();
        Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
        graphics.FillPath(&brush, &path);
    }

    void solidFill(HDC dc, RECT area, COLORREF color) {
        area = paintRect(area);
        const auto brush = CreateSolidBrush(color);
        FillRect(dc, &area, brush);
        DeleteObject(brush);
    }

    void paintSetup(HWND window, HDC target) {
        RECT client{};
        GetClientRect(window, &client);
        const int width = MulDiv(client.right, 96, static_cast<int>(uiDpi));
        const int height = MulDiv(client.bottom, 96, static_cast<int>(uiDpi));
        HDC dc = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
        const auto oldBitmap = SelectObject(dc, bitmap);
        logicalPainting = true;
        solidFill(dc, {0, 0, width, height}, RGB(248, 250, 253));
        solidFill(dc, {0, 0, 220, height}, RGB(20, 31, 53));
        solidFill(dc, {220, 0, width, 4}, RGB(86, 102, 234));
        roundedFill(dc, {30, 30, 66, 66}, RGB(93, 111, 238), 12);
        drawText(dc, L"G", {30, 30, 66, 66}, buttonFont, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        drawText(dc, L"GamEngine", {77, 31, 207, 64}, buttonFont, RGB(255, 255, 255));
        drawText(dc, L"SETUP WIZARD", {31, 111, 195, 135}, smallFont, RGB(135, 154, 186));
        const wchar_t *stepNames[] = {L"Welcome", L"Preferences", L"License", L"Installing", L"Complete"};
        const int active = static_cast<int>(currentStep);
        for (int i = 0; i < 5; ++i) {
            const int y = 150 + i * 59;
            if (i < 4) solidFill(dc, {43, y + 30, 45, y + 56}, RGB(63, 79, 111));
            if (i == active) {
                roundedFill(dc, {20, y - 8, 205, y + 39}, RGB(42, 59, 92), 10);
                roundedFill(dc, {20, y + 1, 23, y + 31}, RGB(110, 128, 255), 2);
            }
            roundedFill(dc, {31, y, 57, y + 26},
                        i <= active ? RGB(99, 117, 246) : RGB(55, 73, 104), 13);
            const auto number = std::to_wstring(i + 1);
            drawText(dc, i < active ? L"\x2713" : number.c_str(), {31, y, 57, y + 26}, smallFont,
                     RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            drawText(dc, stepNames[i], {70, y - 1, 201, y + 27}, bodyFont,
                     i == active ? RGB(255, 255, 255) : RGB(177, 190, 211));
        }
        solidFill(dc, {30, height - 72, 190, height - 71}, RGB(60, 76, 107));
        drawText(dc, L"GamEngine Editor", {30, height - 61, 204, height - 39}, smallFont, RGB(184, 198, 219));
        drawText(dc, L"Official release installer", {30, height - 40, 205, height - 19}, smallFont, RGB(125, 145, 177));

        const wchar_t *eyebrow[] = {L"WELCOME", L"STEP 2 OF 5", L"STEP 3 OF 5", L"STEP 4 OF 5", L"ALL DONE"};
        const wchar_t *heading[] = {
            L"Install GamEngine", L"Choose your preferences", L"Review the license",
            L"Installing GamEngine", L"Ready to create"
        };
        const wchar_t *subtitle[] = {
            L"The editor will be ready in a few simple steps.",
            L"Choose where the editor goes and how to find it.",
            L"Please accept the license before installation.",
            L"Downloading and preparing the latest release.",
            L"GamEngine Editor has been installed successfully."
        };
        drawText(dc, eyebrow[active], {254, 40, 728, 64}, smallFont, RGB(86, 103, 228));
        HFONT titleFont = CreateFontW(-px(30), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                      L"Segoe UI");
        drawText(dc, heading[active], {254, 68, 740, 112}, titleFont, RGB(25, 37, 61));
        DeleteObject(titleFont);
        drawText(dc, subtitle[active], {254, 116, 740, 147}, bodyFont, RGB(91, 106, 133));

        if (currentStep == SetupStep::welcome) {
            roundedFill(dc, {254, 174, 730, 344}, RGB(226, 232, 242), 16);
            roundedFill(dc, {255, 175, 729, 343}, RGB(255, 255, 255), 15);
            roundedFill(dc, {277, 198, 325, 246}, RGB(230, 234, 255), 13);
            drawText(dc, L"G", {277, 198, 325, 246}, buttonFont, RGB(82, 99, 222),
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            drawText(dc, L"Latest official release", {342, 198, 708, 225}, buttonFont, RGB(35, 49, 78));
            drawText(dc, L"GamEngine Editor for Windows", {342, 224, 708, 249},
                     bodyFont, RGB(93, 108, 132));
            solidFill(dc, {277, 267, 706, 268}, RGB(234, 238, 245));
            roundedFill(dc, {277, 284, 303, 310}, RGB(226, 244, 235), 13);
            drawText(dc, L"\x2713", {277, 284, 303, 310}, smallFont, RGB(24, 135, 83),
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            drawText(dc, L"Verified archive from the official release", {316, 283, 708, 311},
                     bodyFont, RGB(73, 89, 117));
        } else if (currentStep == SetupStep::preferences) {
            roundedFill(dc, {254, 174, 730, 418}, RGB(226, 232, 242), 15);
            roundedFill(dc, {255, 175, 729, 417}, RGB(255, 255, 255), 14);
            drawText(dc, L"Installation folder", {277, 191, 706, 220}, buttonFont, RGB(35, 49, 78));
            drawText(dc, L"Choose a dedicated folder for GamEngine.", {277, 217, 706, 239}, smallFont,
                     RGB(101, 116, 140));
            roundedFill(dc, {276, 250, 615, 290}, RGB(213, 222, 236), 9);
            roundedFill(dc, {277, 251, 614, 289}, RGB(255, 255, 255), 8);
            solidFill(dc, {277, 313, 706, 314}, RGB(234, 238, 245));
            drawText(dc, L"SHORTCUTS", {277, 321, 706, 345}, smallFont, RGB(88, 104, 137));
        } else if (currentStep == SetupStep::license) {
            roundedFill(dc, {254, 174, 730, 439}, RGB(226, 232, 242), 14);
            roundedFill(dc, {255, 175, 729, 438}, RGB(255, 255, 255), 13);
            drawText(dc, L"Apache License 2.0", {277, 184, 706, 212}, buttonFont, RGB(35, 49, 78));
            roundedFill(dc, {276, 215, 708, 389}, RGB(225, 231, 241), 8);
            solidFill(dc, {277, 399, 706, 400}, RGB(234, 238, 245));
        } else if (currentStep == SetupStep::progress || currentStep == SetupStep::complete) {
            roundedFill(dc, {254, 174, 730, 329}, RGB(226, 232, 242), 14);
            roundedFill(dc, {255, 175, 729, 328}, RGB(255, 255, 255), 13);
            drawText(dc, currentStep == SetupStep::complete ? L"Installation complete" : L"Setting up your editor",
                     {277, 190, 706, 224}, buttonFont, RGB(35, 49, 78));
            const auto percent = std::to_wstring(progressValue) + L"%";
            drawText(dc, percent.c_str(), {636, 261, 706, 286}, smallFont, RGB(86, 103, 228),
                     DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            roundedFill(dc, {277, 292, 707, 302}, RGB(224, 230, 240), 5);
            if (progressValue > 0) {
                const int right = 277 + 430 * std::clamp(progressValue, 0, 100) / 100;
                roundedFill(dc, {277, 292, right, 302}, RGB(86, 103, 228), 5);
            }
            if (currentStep == SetupStep::complete) {
                roundedFill(dc, {254, 347, 730, 432}, RGB(226, 232, 242), 14);
                roundedFill(dc, {255, 348, 729, 431}, RGB(255, 255, 255), 13);
                solidFill(dc, {277, 385, 706, 386}, RGB(234, 238, 245));
            }
        }
        if (currentStep == SetupStep::welcome) {
            roundedFill(dc, {254, 361, 730, 412}, RGB(226, 232, 242), 12);
            roundedFill(dc, {255, 362, 729, 411}, RGB(255, 255, 255), 11);
        }
        solidFill(dc, {220, 451, width, 452}, RGB(226, 232, 241));
        solidFill(dc, {220, 452, width, height}, RGB(255, 255, 255));
        logicalPainting = false;
        BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }

    std::wstring embeddedLicense() {
        const auto resource = FindResourceW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), RT_RCDATA);
        if (!resource) return L"The license text is unavailable.";
        const auto handle = LoadResource(GetModuleHandleW(nullptr), resource);
        const auto bytes = static_cast<const char *>(LockResource(handle));
        const int size = static_cast<int>(SizeofResource(GetModuleHandleW(nullptr), resource));
        if (!bytes || size <= 0) return L"The license text is unavailable.";
        const int length = MultiByteToWideChar(CP_UTF8, 0, bytes, size, nullptr, 0);
        std::wstring license(length, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, bytes, size, license.data(), length);
        std::wstring windowsText;
        windowsText.reserve(license.size() * 2);
        for (size_t i = 0; i < license.size(); ++i) {
            if (license[i] == L'\n' && (i == 0 || license[i - 1] != L'\r')) windowsText += L'\r';
            windowsText += license[i];
        }
        return windowsText;
    }

    void browseFolder(HWND parent) {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IFileOpenDialog *dialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
            DWORD options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
            dialog->SetTitle(L"Choose installation folder");
            if (SUCCEEDED(dialog->Show(parent))) {
                IShellItem *item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item))) {
                    PWSTR selected = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &selected))) {
                        SetWindowTextW(pathEdit, selected);
                        CoTaskMemFree(selected);
                    }
                    item->Release();
                }
            }
            dialog->Release();
        }
        if (SUCCEEDED(init)) CoUninitialize();
    }

    void refreshFonts(HWND window) {
        const auto oldBody = bodyFont;
        const auto oldSmall = smallFont;
        const auto oldButton = buttonFont;
        const auto oldLink = linkFont;
        bodyFont = CreateFontW(-px(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        smallFont = CreateFontW(-px(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        buttonFont = CreateFontW(-px(16), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                 L"Segoe UI");
        linkFont = CreateFontW(-px(16), 0, 0, 0, FW_SEMIBOLD, FALSE, TRUE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        EnumChildWindows(window, [](HWND child, LPARAM) -> BOOL {
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont), TRUE);
            return TRUE;
        }, 0);
        SendMessageW(installButton, WM_SETFONT, reinterpret_cast<WPARAM>(buttonFont), TRUE);
        SendMessageW(browseButton, WM_SETFONT, reinterpret_cast<WPARAM>(buttonFont), TRUE);
        DeleteObject(oldBody);
        DeleteObject(oldSmall);
        DeleteObject(oldButton);
        DeleteObject(oldLink);
    }

    void layoutSetup(HWND window) {
        MoveWindow(pathEdit, px(287), px(259), px(317), px(22), TRUE);
        MoveWindow(browseButton, px(625), px(250), px(82), px(40), TRUE);
        MoveWindow(desktopCheck, px(277), px(350), px(400), px(30), TRUE);
        MoveWindow(startCheck, px(277), px(383), px(400), px(30), TRUE);
        MoveWindow(consentCheck, px(277), px(402), px(430), px(32), TRUE);
        MoveWindow(licenseText, px(281), px(221), px(422), px(163), TRUE);
        MoveWindow(projectButton, px(277), px(currentStep == SetupStep::complete ? 351 : 371),
                   px(430), px(32), TRUE);
        MoveWindow(runCheck, px(277), px(389), px(430), px(32), TRUE);
        MoveWindow(statusLabel, px(277), px(231), px(430), px(30), TRUE);
        MoveWindow(backButton, px(254), px(469), px(105), px(42), TRUE);
        MoveWindow(installButton, px(575), px(469), px(155), px(42), TRUE);
        refreshFonts(window);
        SendMessageW(licenseText, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(px(9), px(9)));
        InvalidateRect(window, nullptr, TRUE);
    }

    void resizeForDpi(HWND window, const RECT *suggested, int clientWidth, int clientHeight) {
        const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
        const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
        RECT bounds{0, 0, px(clientWidth), px(clientHeight)};
        AdjustWindowRectExForDpi(&bounds, style, FALSE, exStyle, uiDpi);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
                     bounds.right - bounds.left, bounds.bottom - bounds.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void showStep(HWND window, SetupStep step) {
        currentStep = step;
        projectHovered = false;
        const bool preferences = step == SetupStep::preferences;
        const bool license = step == SetupStep::license;
        const bool progress = step == SetupStep::progress;
        const bool complete = step == SetupStep::complete;
        for (HWND control: {pathEdit, browseButton, desktopCheck, startCheck})
            ShowWindow(control, preferences ? SW_SHOW : SW_HIDE);
        for (HWND control: {consentCheck, licenseText})
            ShowWindow(control, license ? SW_SHOW : SW_HIDE);
        EnableWindow(consentCheck, licenseAvailable && !installing);
        ShowWindow(projectButton, step == SetupStep::welcome || complete ? SW_SHOW : SW_HIDE);
        if (step == SetupStep::welcome || complete)
            MoveWindow(projectButton, px(277), px(complete ? 351 : 371), px(430), px(32), TRUE);
        ShowWindow(runCheck, complete ? SW_SHOW : SW_HIDE);
        ShowWindow(statusLabel, progress || complete ? SW_SHOW : SW_HIDE);
        ShowWindow(backButton, preferences || license ? SW_SHOW : SW_HIDE);
        SetWindowTextW(installButton, complete ? L"Finish" : license ? L"Install" : L"Continue");
        EnableWindow(installButton, !progress && (!license || consentSelected));
        InvalidateRect(window, nullptr, FALSE);
    }

    void scrollLicense(WPARAM wheel) {
        licenseWheelRemainder += GET_WHEEL_DELTA_WPARAM(wheel) * 9;
        const int lines = licenseWheelRemainder / WHEEL_DELTA;
        licenseWheelRemainder %= WHEEL_DELTA;
        if (lines != 0) SendMessageW(licenseText, EM_LINESCROLL, 0, -lines);
    }

    LRESULT CALLBACK licenseTextProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                     UINT_PTR subclassId, DWORD_PTR) {
        if (message == WM_MOUSEWHEEL) {
            scrollLicense(wparam);
            return 0;
        }
        if (message == WM_NCDESTROY) RemoveWindowSubclass(window, licenseTextProc, subclassId);
        return DefSubclassProc(window, message, wparam, lparam);
    }

    LRESULT CALLBACK projectButtonProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                       UINT_PTR subclassId, DWORD_PTR) {
        if (message == WM_MOUSEMOVE && !projectHovered) {
            projectHovered = true;
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
            TrackMouseEvent(&tracking);
            InvalidateRect(window, nullptr, TRUE);
        } else if (message == WM_MOUSELEAVE) {
            projectHovered = false;
            InvalidateRect(window, nullptr, TRUE);
        } else if (message == WM_SETCURSOR) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        } else if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(window, projectButtonProc, subclassId);
        }
        return DefSubclassProc(window, message, wparam, lparam);
    }

    LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
            case WM_CREATE: {
                whiteBrush = CreateSolidBrush(RGB(255, 255, 255));
                cardBrush = CreateSolidBrush(RGB(255, 255, 255));
                pathEdit = CreateWindowW(L"EDIT", (fs::path(localAppData()) / L"Programs" / L"GamEngine").c_str(),
                                         WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 287, 259, 317, 22, window,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_PATH)), nullptr, nullptr);
                browseButton = CreateWindowW(L"BUTTON", L"Browse", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                             625, 250, 82, 40, window,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BROWSE)), nullptr,
                                             nullptr);
                desktopCheck = CreateWindowW(L"BUTTON", L"Create desktop shortcut",
                                             WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                             277, 350, 400, 30, window,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_DESKTOP)), nullptr,
                                             nullptr);
                startCheck = CreateWindowW(L"BUTTON", L"Add to Start menu", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                           277, 383, 400, 30, window,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_START)), nullptr, nullptr);
                consentCheck = CreateWindowW(L"BUTTON", L"I have read and accept the Apache License 2.0",
                                             WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 277, 402, 430, 32, window,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CONSENT)), nullptr,
                                             nullptr);
                const auto license = embeddedLicense();
                licenseAvailable = !license.empty() && license != L"The license text is unavailable.";
                const auto richEdit = LoadLibraryW(L"Msftedit.dll");
                licenseText = CreateWindowW(richEdit ? MSFTEDIT_CLASS : L"STATIC", license.c_str(),
                                            WS_CHILD | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                                            ES_AUTOVSCROLL, 281, 221, 422, 163, window,
                                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_LICENSE_TEXT)), nullptr,
                                            nullptr);
                licenseAvailable = licenseAvailable && richEdit && licenseText;
                if (richEdit) SendMessageW(licenseText, EM_SETBKGNDCOLOR, 0, RGB(255, 255, 255));
                projectButton = CreateWindowW(L"BUTTON", L"Visit project page",
                                              WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 277, 371, 430, 32, window,
                                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_PROJECT)), nullptr,
                                              nullptr);
                runCheck = CreateWindowW(L"BUTTON", L"Run GamEngine Editor after installation",
                                         WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 277, 389, 430, 32, window,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_RUN)), nullptr, nullptr);
                statusLabel = CreateWindowW(L"STATIC", L"Preparing installation...", WS_CHILD,
                                            277, 231, 430, 30, window, nullptr, nullptr, nullptr);
                backButton = CreateWindowW(L"BUTTON", L"Back", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                           254, 469, 105, 42, window,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BACK)), nullptr, nullptr);
                installButton = CreateWindowW(L"BUTTON", L"Continue", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                              575, 469, 155, 42, window,
                                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_INSTALL)), nullptr,
                                              nullptr);
                SetWindowSubclass(licenseText, licenseTextProc, 1, 0);
                SetWindowSubclass(projectButton, projectButtonProc, 1, 0);
                uiDpi = GetDpiForWindow(window);
                layoutSetup(window);
                showStep(window, SetupStep::welcome);
                return 0;
            }
            case WM_DPICHANGED:
                uiDpi = HIWORD(wparam);
                resizeForDpi(window, reinterpret_cast<const RECT *>(lparam), 764, 530);
                layoutSetup(window);
                return 0;
            case WM_MOUSEWHEEL:
                if (currentStep == SetupStep::license) {
                    scrollLicense(wparam);
                    return 0;
                }
                break;
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: {
                PAINTSTRUCT paint{};
                HDC dc = BeginPaint(window, &paint);
                paintSetup(window, dc);
                EndPaint(window, &paint);
                return 0;
            }
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLORBTN:
                SetBkMode(reinterpret_cast<HDC>(wparam), TRANSPARENT);
                SetTextColor(reinterpret_cast<HDC>(wparam),
                             installationFailed && reinterpret_cast<HWND>(lparam) == statusLabel
                                 ? RGB(185, 47, 58)
                                 : RGB(42, 53, 74));
                if (reinterpret_cast<HWND>(lparam) == statusLabel) return reinterpret_cast<LRESULT>(cardBrush);
                return reinterpret_cast<LRESULT>(whiteBrush);
            case WM_CTLCOLOREDIT:
                SetBkColor(reinterpret_cast<HDC>(wparam), RGB(255, 255, 255));
                SetTextColor(reinterpret_cast<HDC>(wparam), RGB(42, 53, 74));
                return reinterpret_cast<LRESULT>(whiteBrush);
            case WM_DRAWITEM: {
                const auto item = reinterpret_cast<DRAWITEMSTRUCT *>(lparam);
                if (item->CtlID != ID_INSTALL && item->CtlID != ID_BACK && item->CtlID != ID_BROWSE && item->CtlID !=
                    ID_PROJECT && item->CtlID != ID_DESKTOP && item->CtlID != ID_START &&
                    item->CtlID != ID_CONSENT && item->CtlID != ID_RUN)
                    break;
                RECT rect = item->rcItem;
                const bool enabled = (item->itemState & ODS_DISABLED) == 0;
                if (item->CtlID == ID_INSTALL) {
                    solidFill(item->hDC, rect, RGB(255, 255, 255));
                    roundedFill(item->hDC, rect, enabled ? RGB(75, 92, 220) : RGB(181, 190, 213), px(11));
                    wchar_t caption[100]{};
                    GetWindowTextW(item->hwndItem, caption, 100);
                    drawText(item->hDC, caption, rect, buttonFont, RGB(255, 255, 255),
                             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                } else if (item->CtlID == ID_BACK) {
                    solidFill(item->hDC, rect, RGB(255, 255, 255));
                    roundedFill(item->hDC, rect, RGB(239, 242, 248), px(10));
                    drawText(item->hDC, L"Back", rect, buttonFont, RGB(54, 70, 103),
                             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                } else if (item->CtlID == ID_BROWSE) {
                    solidFill(item->hDC, rect, RGB(255, 255, 255));
                    roundedFill(item->hDC, rect, enabled ? RGB(235, 239, 250) : RGB(246, 247, 250), px(9));
                    drawText(item->hDC, L"Browse", rect, buttonFont, enabled ? RGB(56, 71, 108) : RGB(160, 166, 179),
                             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                } else if (item->CtlID == ID_PROJECT) {
                    FillRect(item->hDC, &rect, whiteBrush);
                    drawText(item->hDC, L"Visit project page  \x2197", rect, linkFont,
                             projectHovered || (item->itemState & ODS_FOCUS)
                                 ? RGB(45, 61, 185)
                                 : RGB(74, 91, 221));
                } else {
                    FillRect(item->hDC, &rect, whiteBrush);
                    if (item->itemState & ODS_FOCUS)
                        roundedFill(item->hDC, rect, RGB(239, 242, 255), px(8));
                    const bool checked = item->CtlID == ID_DESKTOP
                                             ? desktopSelected
                                             : item->CtlID == ID_START
                                                   ? startSelected
                                                   : item->CtlID == ID_RUN
                                                         ? runAfterInstall
                                                         : consentSelected;
                    RECT checkBox{rect.left + px(1), rect.top + px(3), rect.left + px(20), rect.top + px(22)};
                    roundedFill(item->hDC, checkBox, checked ? RGB(75, 92, 220) : RGB(198, 209, 227), px(6));
                    if (!checked)
                        roundedFill(item->hDC, {
                                        checkBox.left + px(1), checkBox.top + px(1),
                                        checkBox.right - px(1), checkBox.bottom - px(1)
                                    }, RGB(255, 255, 255), px(5));
                    if (checked) {
                        const auto checkPen = CreatePen(PS_SOLID, px(2), RGB(255, 255, 255));
                        const auto previousPen = SelectObject(item->hDC, checkPen);
                        MoveToEx(item->hDC, rect.left + px(5), rect.top + px(12), nullptr);
                        LineTo(item->hDC, rect.left + px(9), rect.top + px(16));
                        LineTo(item->hDC, rect.left + px(16), rect.top + px(8));
                        SelectObject(item->hDC, previousPen);
                        DeleteObject(checkPen);
                    }
                    wchar_t caption[128]{};
                    GetWindowTextW(item->hwndItem, caption, 128);
                    drawText(item->hDC, caption, {rect.left + px(31), rect.top, rect.right, rect.bottom}, bodyFont,
                             enabled ? RGB(42, 53, 74) : RGB(154, 163, 181));
                }
                return TRUE;
            }
            case WM_COMMAND:
                if (HIWORD(wparam) != BN_CLICKED) return 0;
                if (LOWORD(wparam) == ID_BACK && !installing) {
                    if (currentStep == SetupStep::license) showStep(window, SetupStep::preferences);
                    else if (currentStep == SetupStep::preferences) showStep(window, SetupStep::welcome);
                    return 0;
                }
                if (LOWORD(wparam) == ID_BROWSE && !installing) {
                    browseFolder(window);
                    return 0;
                }
                if (LOWORD(wparam) == ID_PROJECT) {
                    if (reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open",
                                                                L"https://github.com/B4rtekk1/GamEngine", nullptr,
                                                                nullptr, SW_SHOWNORMAL)) <= 32)
                        MessageBoxW(window, L"Could not open the project page.", L"GamEngine Setup",
                                    MB_OK | MB_ICONERROR);
                    return 0;
                }
                if (LOWORD(wparam) == ID_RUN) {
                    runAfterInstall = !runAfterInstall;
                    InvalidateRect(runCheck, nullptr, TRUE);
                    return 0;
                }
                if (LOWORD(wparam) == ID_DESKTOP && !installing) {
                    desktopSelected = !desktopSelected;
                    InvalidateRect(desktopCheck, nullptr, TRUE);
                    return 0;
                }
                if (LOWORD(wparam) == ID_START && !installing) {
                    startSelected = !startSelected;
                    InvalidateRect(startCheck, nullptr, TRUE);
                    return 0;
                }
                if (LOWORD(wparam) == ID_CONSENT) {
                    if (licenseAvailable && !installing) consentSelected = !consentSelected;
                    InvalidateRect(consentCheck, nullptr, TRUE);
                    EnableWindow(installButton, !installing && consentSelected);
                    return 0;
                }
                if (LOWORD(wparam) == ID_INSTALL && currentStep == SetupStep::complete) {
                    if (runAfterInstall) {
                        try {
                            launchEditor(fs::path(editText(pathEdit)));
                        } catch (const std::exception &error) {
                            MessageBoxA(window, error.what(), "Could not start GamEngine Editor",
                                        MB_OK | MB_ICONERROR);
                            return 0;
                        }
                    }
                    DestroyWindow(window);
                    return 0;
                }
                if (LOWORD(wparam) == ID_INSTALL && currentStep == SetupStep::welcome) {
                    showStep(window, SetupStep::preferences);
                    return 0;
                }
                if (LOWORD(wparam) == ID_INSTALL && currentStep == SetupStep::preferences) {
                    try {
                        const auto root = editText(pathEdit);
                        if (root.empty()) throw std::runtime_error("Choose an installation folder");
                        validateRoot(fs::path(root));
                    } catch (const std::exception &error) {
                        MessageBoxA(window, error.what(), "Installation folder", MB_OK | MB_ICONWARNING);
                        return 0;
                    }
                    showStep(window, SetupStep::license);
                    return 0;
                }
                if (LOWORD(wparam) == ID_INSTALL && (currentStep == SetupStep::license ||
                                                     (currentStep == SetupStep::progress && installationFailed)) &&
                    consentSelected && !installing.exchange(true)) {
                    const auto root = editText(pathEdit);
                    const bool desktop = desktopSelected;
                    const bool start = startSelected;
                    installationFailed = false;
                    progressValue = 0;
                    EnableWindow(installButton, FALSE);
                    EnableWindow(pathEdit, FALSE);
                    EnableWindow(browseButton, FALSE);
                    EnableWindow(desktopCheck, FALSE);
                    EnableWindow(startCheck, FALSE);
                    EnableWindow(consentCheck, FALSE);
                    showStep(window, SetupStep::progress);
                    std::thread([root, desktop, start] {
                        try {
                            installTo(root, desktop, start);
                            PostMessageW(mainWindow, WM_SETUP_DONE, 1, 0);
                        } catch (const std::exception &error) {
                            std::wstring message(error.what(), error.what() + std::strlen(error.what()));
                            PostMessageW(mainWindow, WM_SETUP_DONE, 0,
                                         reinterpret_cast<LPARAM>(new std::wstring(message)));
                        }
                    }).detach();
                }
                return 0;
            case WM_SETUP_STATUS:
                SetWindowTextW(statusLabel, reinterpret_cast<std::wstring *>(lparam)->c_str());
                delete reinterpret_cast<std::wstring *>(lparam);
                return 0;
            case WM_SETUP_PROGRESS:
                progressValue = static_cast<int>(wparam);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            case WM_SETUP_DONE:
                installing = false;
                if (wparam) {
                    progressValue = 100;
                    SetWindowTextW(statusLabel, L"GamEngine is ready to use.");
                    showStep(window, SetupStep::complete);
                } else {
                    const auto error = reinterpret_cast<std::wstring *>(lparam);
                    SetWindowTextW(statusLabel, L"Installation failed. You can retry.");
                    MessageBoxW(window, error->c_str(), L"GamEngine installation failed", MB_OK | MB_ICONERROR);
                    delete error;
                    installationFailed = true;
                    EnableWindow(installButton, TRUE);
                    EnableWindow(pathEdit, TRUE);
                    EnableWindow(browseButton, TRUE);
                    EnableWindow(desktopCheck, TRUE);
                    EnableWindow(startCheck, TRUE);
                    EnableWindow(consentCheck, TRUE);
                    showStep(window, SetupStep::progress);
                    EnableWindow(installButton, TRUE);
                    SetWindowTextW(installButton, L"Retry");
                }
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            case WM_CLOSE:
                if (!installing) DestroyWindow(window);
                return 0;
            case WM_DESTROY:
                DeleteObject(bodyFont);
                DeleteObject(smallFont);
                DeleteObject(buttonFont);
                DeleteObject(linkFont);
                DeleteObject(whiteBrush);
                DeleteObject(cardBrush);
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    try {
        int argc = 0;
        LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv) throw std::runtime_error("Cannot read command line");
        const auto root = fs::path(modulePath()).parent_path();
        const std::wstring command = argc > 1 ? argv[1] : L"";
        if (command == L"--launch") {
            LocalFree(argv);
            launchEditor(root);
            return 0;
        }
        if (command == L"--rollback") {
            LocalFree(argv);
            rollback(root);
            return 0;
        }
        if (command == L"--verify-release" && argc == 3) {
            const auto destination = fs::path(argv[2]);
            LocalFree(argv);
            const auto asset = latestEditorRelease();
            downloadEditor(asset, destination, [](int) {
            });
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
                            MB_YESNO | MB_ICONQUESTION) == IDYES)
                startUninstall(root);
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
        type.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        type.hIconSm = type.hIcon;
        type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassExW(&type);
        uiDpi = GetDpiForSystem();
        RECT bounds{0, 0, px(764), px(530)};
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        AdjustWindowRectExForDpi(&bounds, style, FALSE, 0, uiDpi);
        mainWindow = CreateWindowExW(0, type.lpszClassName, L"GamEngine Setup",
                                     style, CW_USEDEFAULT, CW_USEDEFAULT,
                                     bounds.right - bounds.left, bounds.bottom - bounds.top,
                                     nullptr, nullptr, instance, nullptr);
        if (!mainWindow) throw std::runtime_error("Cannot create setup window");
        RECT actualBounds{};
        GetWindowRect(mainWindow, &actualBounds);
        uiDpi = GetDpiForWindow(mainWindow);
        resizeForDpi(mainWindow, &actualBounds, 764, 530);
        const DWORD corners = 2; // DWMWCP_ROUND on Windows 11; ignored on older releases.
        DwmSetWindowAttribute(mainWindow, 33, &corners, sizeof(corners));
        ShowWindow(mainWindow, show);
        UpdateWindow(mainWindow);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    } catch (const std::exception &error) {
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
            } catch (...) {
            }
            return 1;
        }
        MessageBoxA(nullptr, error.what(), "GamEngine Setup", MB_OK | MB_ICONERROR);
        return 1;
    }
}
