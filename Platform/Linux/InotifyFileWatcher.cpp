#include "Platform/FileWatcher.h"

#include <sys/inotify.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace Platform {
    class FileWatcher::Impl final {
    public:
        explicit Impl(std::filesystem::path directory) { watch(std::move(directory)); }
        ~Impl() {
            if (descriptor >= 0) close(descriptor);
        }

        void watch(std::filesystem::path directory) {
            if (descriptor >= 0) close(descriptor);
            descriptor = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
            root = std::move(directory);
            watches.clear();
            if (descriptor >= 0 && std::filesystem::is_directory(root)) addDirectoryTree(root);
        }

        [[nodiscard]] std::vector<FileChange> poll() {
            if (descriptor < 0) return {};
            std::array<std::byte, 16 * 1024> buffer{};
            for (;;) {
                const auto bytes = read(descriptor, buffer.data(), buffer.size());
                if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                if (bytes <= 0) break;
                for (std::size_t offset = 0; offset < static_cast<std::size_t>(bytes);) {
                    const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
                    const auto it = watches.find(event->wd);
                    if (it != watches.end() && event->len > 0) {
                        const auto path = it->second / event->name;
                        if ((event->mask & IN_ISDIR) != 0 && (event->mask & (IN_CREATE | IN_MOVED_TO)) != 0) {
                            addDirectoryTree(path);
                        }
                        const auto extension = path.extension().string();
                        if (extension == ".cpp" || extension == ".h") {
                            changes.push_back({path, changeType(event->mask)});
                        }
                    }
                    offset += sizeof(inotify_event) + event->len;
                }
            }
            return std::exchange(changes, {});
        }

    private:
        static constexpr std::uint32_t flags = IN_CLOSE_WRITE | IN_CREATE | IN_DELETE |
                                               IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB;

        static FileChangeType changeType(const std::uint32_t mask) {
            if ((mask & IN_CREATE) != 0) return FileChangeType::added;
            if ((mask & IN_DELETE) != 0) return FileChangeType::removed;
            if ((mask & (IN_MOVED_FROM | IN_MOVED_TO)) != 0) return FileChangeType::renamed;
            return FileChangeType::modified;
        }

        void addDirectoryTree(const std::filesystem::path& directory) {
            if (!std::filesystem::is_directory(directory)) return;
            const int watch = inotify_add_watch(descriptor, directory.c_str(), flags);
            if (watch >= 0) watches.emplace(watch, directory);
            std::error_code error;
            for (std::filesystem::recursive_directory_iterator iterator{directory, error}, end;
                 !error && iterator != end; iterator.increment(error)) {
                if (iterator->is_directory(error)) addDirectoryTree(iterator->path());
            }
        }

        int descriptor{-1};
        std::filesystem::path root;
        std::unordered_map<int, std::filesystem::path> watches;
        std::vector<FileChange> changes;
    };

    FileWatcher::FileWatcher(std::filesystem::path root) : impl_{std::make_unique<Impl>(std::move(root))} {}
    FileWatcher::~FileWatcher() = default;
    void FileWatcher::watch(std::filesystem::path root) { impl_->watch(std::move(root)); }
    std::vector<FileChange> FileWatcher::poll() { return impl_->poll(); }
} // namespace Platform
