#include "Platform/FileWatcher.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

namespace Platform {
    class FileWatcher::Impl final {
    public:
        explicit Impl(std::filesystem::path directory) { start(std::move(directory)); }
        ~Impl() { stop(); }

        void watch(std::filesystem::path directory) {
            stop();
            start(std::move(directory));
        }

        [[nodiscard]] std::vector<FileChange> poll() {
            std::scoped_lock lock{changesMutex};
            return std::exchange(changes, {});
        }

    private:
        static FileChangeType changeType(const DWORD action) {
            switch (action) {
            case FILE_ACTION_ADDED: return FileChangeType::added;
            case FILE_ACTION_REMOVED: return FileChangeType::removed;
            case FILE_ACTION_RENAMED_OLD_NAME:
            case FILE_ACTION_RENAMED_NEW_NAME: return FileChangeType::renamed;
            default: return FileChangeType::modified;
            }
        }

        void start(std::filesystem::path directory) {
            if (!std::filesystem::is_directory(directory)) return;
            stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            directoryHandle = CreateFileW(directory.c_str(), FILE_LIST_DIRECTORY,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr, OPEN_EXISTING,
                                          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
            if (stopEvent == nullptr || directoryHandle == INVALID_HANDLE_VALUE) {
                if (directoryHandle != INVALID_HANDLE_VALUE) CloseHandle(directoryHandle);
                if (stopEvent != nullptr) CloseHandle(stopEvent);
                directoryHandle = INVALID_HANDLE_VALUE;
                stopEvent = nullptr;
                return;
            }
            root = std::move(directory);
            worker = std::jthread([this] { run(); });
        }

        void stop() {
            if (!worker.joinable()) return;
            SetEvent(stopEvent);
            CancelIoEx(directoryHandle, nullptr);
            worker.join();
            CloseHandle(directoryHandle);
            CloseHandle(stopEvent);
            directoryHandle = INVALID_HANDLE_VALUE;
            stopEvent = nullptr;
        }

        void run() {
            std::array<std::byte, 16 * 1024> buffer{};
            constexpr DWORD filters = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                      FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION |
                                      FILE_NOTIFY_CHANGE_SIZE;
            for (;;) {
                OVERLAPPED overlapped{};
                overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (overlapped.hEvent == nullptr) return;
                const BOOL requested = ReadDirectoryChangesW(
                    directoryHandle, buffer.data(), static_cast<DWORD>(buffer.size()), TRUE, filters,
                    nullptr, &overlapped, nullptr);
                if (!requested) {
                    CloseHandle(overlapped.hEvent);
                    return;
                }
                HANDLE events[]{stopEvent, overlapped.hEvent};
                const DWORD waitResult = WaitForMultipleObjects(2, events, FALSE, INFINITE);
                if (waitResult == WAIT_OBJECT_0) {
                    CancelIoEx(directoryHandle, &overlapped);
                    CloseHandle(overlapped.hEvent);
                    return;
                }

                DWORD bytesTransferred{};
                if (waitResult == WAIT_OBJECT_0 + 1 &&
                    GetOverlappedResult(directoryHandle, &overlapped, &bytesTransferred, FALSE)) {
                    auto* notification = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer.data());
                    for (;;) {
                        const std::wstring_view name{notification->FileName,
                                                     notification->FileNameLength / sizeof(WCHAR)};
                        const auto path = root / name;
                        const auto extension = path.extension().wstring();
                        if (extension == L".cpp" || extension == L".h") {
                            std::scoped_lock lock{changesMutex};
                            changes.push_back({path, changeType(notification->Action)});
                        }
                        if (notification->NextEntryOffset == 0) break;
                        notification = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
                            reinterpret_cast<std::byte*>(notification) + notification->NextEntryOffset);
                    }
                }
                CloseHandle(overlapped.hEvent);
            }
        }

        std::filesystem::path root;
        HANDLE directoryHandle{INVALID_HANDLE_VALUE};
        HANDLE stopEvent{};
        std::jthread worker;
        std::mutex changesMutex;
        std::vector<FileChange> changes;
    };

    FileWatcher::FileWatcher(std::filesystem::path root) : impl_{std::make_unique<Impl>(std::move(root))} {}
    FileWatcher::~FileWatcher() = default;
    void FileWatcher::watch(std::filesystem::path root) { impl_->watch(std::move(root)); }
    std::vector<FileChange> FileWatcher::poll() { return impl_->poll(); }
} // namespace Platform
