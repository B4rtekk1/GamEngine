#include "Platform/Terminal/TerminalSession.h"

#include <Windows.h>
#include <array>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace {

void closeHandle(HANDLE& handle) {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    handle = nullptr;
}

bool existsOnPath(const wchar_t* name) {
    wchar_t result[MAX_PATH]{};
    return SearchPathW(nullptr, name, nullptr, static_cast<DWORD>(std::size(result)), result, nullptr) > 0;
}

class ConPtyTerminal final : public Platform::TerminalSession {
public:
    ~ConPtyTerminal() override { stop(); }

    bool start(const std::filesystem::path& executable,
               const std::filesystem::path& workingDirectory) override {
        stop();
        if (!CreatePipe(&inputRead_, &inputWrite_, nullptr, 0) ||
            !CreatePipe(&outputRead_, &outputWrite_, nullptr, 0)) {
            stop();
            return false;
        }
        const COORD size{120, 30};
        if (FAILED(CreatePseudoConsole(size, inputRead_, outputWrite_, 0, &pseudoConsole_))) {
            stop();
            return false;
        }
        // The pseudoconsole owns these pipe ends after successful creation.
        closeHandle(inputRead_);
        closeHandle(outputWrite_);

        SIZE_T attributeListSize{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeListSize);
        attributeList_ = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
            HeapAlloc(GetProcessHeap(), 0, attributeListSize));
        if (attributeList_ == nullptr ||
            !InitializeProcThreadAttributeList(attributeList_, 1, 0, &attributeListSize) ||
            !UpdateProcThreadAttribute(attributeList_, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                        pseudoConsole_, sizeof(pseudoConsole_), nullptr, nullptr)) {
            stop();
            return false;
        }
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attributeList_;
        std::wstring command = L"\"" + executable.wstring() + L"\"";
        if (_wcsicmp(executable.filename().c_str(), L"cmd.exe") != 0) command += L" -NoLogo";
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                            EXTENDED_STARTUPINFO_PRESENT, nullptr,
                            workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
                            &startup.StartupInfo, &process)) {
            stop();
            return false;
        }
        processHandle_ = process.hProcess;
        closeHandle(process.hThread);
        readerThread_ = std::jthread([this](const std::stop_token stopToken) { readLoop(stopToken); });
        return true;
    }

    void write(const std::string_view data) override {
        if (inputWrite_ == nullptr || data.empty()) return;
        DWORD written{};
        static_cast<void>(WriteFile(inputWrite_, data.data(), static_cast<DWORD>(data.size()), &written, nullptr));
    }

    [[nodiscard]] std::string readAvailable() override {
        std::scoped_lock lock{outputMutex_};
        return std::exchange(pendingOutput_, {});
    }

    void resize(const std::uint16_t columns, const std::uint16_t rows) override {
        if (pseudoConsole_ != nullptr) {
            static_cast<void>(ResizePseudoConsole(pseudoConsole_,
                                                  {static_cast<SHORT>(columns), static_cast<SHORT>(rows)}));
        }
    }

    [[nodiscard]] bool running() const override {
        if (processHandle_ == nullptr) return false;
        return WaitForSingleObject(processHandle_, 0) == WAIT_TIMEOUT;
    }

    void stop() override {
        closeHandle(inputWrite_);
        closeHandle(inputRead_);
        closeHandle(outputWrite_);

        // Close the ConPTY while the reader is alive: it can drain final output
        // emitted as the attached process tree is asked to terminate.
        if (pseudoConsole_ != nullptr) {
            ClosePseudoConsole(pseudoConsole_);
            pseudoConsole_ = nullptr;
        }

        if (readerThread_.joinable()) {
            readerThread_.request_stop();
            // A stop token cannot interrupt a synchronous ReadFile. Explicitly
            // cancel any read pending on the terminal's reader thread.
            static_cast<void>(CancelSynchronousIo(readerThread_.native_handle()));
            readerThread_.join();
        }
        closeHandle(outputRead_);
        if (attributeList_ != nullptr) {
            DeleteProcThreadAttributeList(attributeList_);
            HeapFree(GetProcessHeap(), 0, attributeList_);
            attributeList_ = nullptr;
        }
        closeHandle(processHandle_);
    }

private:
    void readLoop(const std::stop_token stopToken) {
        std::array<char, 4096> buffer{};
        while (!stopToken.stop_requested()) {
            DWORD read{};
            if (!ReadFile(outputRead_, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || read == 0) break;
            std::scoped_lock lock{outputMutex_};
            pendingOutput_.append(buffer.data(), read);
        }
    }

    HPCON pseudoConsole_{};
    HANDLE inputRead_{};
    HANDLE inputWrite_{};
    HANDLE outputRead_{};
    HANDLE outputWrite_{};
    HANDLE processHandle_{};
    LPPROC_THREAD_ATTRIBUTE_LIST attributeList_{};
    std::jthread readerThread_;
    std::mutex outputMutex_;
    std::string pendingOutput_;
};

} // namespace

namespace Platform {

std::unique_ptr<TerminalSession> createTerminalSession() {
    return std::make_unique<ConPtyTerminal>();
}

std::filesystem::path defaultTerminalShell() {
    if (existsOnPath(L"pwsh.exe")) return L"pwsh.exe";
    if (existsOnPath(L"powershell.exe")) return L"powershell.exe";
    return L"cmd.exe";
}

} // namespace Platform
