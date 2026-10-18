#include "Platform/FileWatcher.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <sys/inotify.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>

namespace Platform {
class FileWatcher::Impl final {
  public:
    explicit Impl(std::filesystem::path directory) {
        watch(std::move(directory));
    }
    ~Impl() {
        if (descriptor >= 0)
            close(descriptor);
    }

    void watch(std::filesystem::path directory) {
        if (descriptor >= 0)
            close(descriptor);
        descriptor = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        root = std::move(directory);
        watches.clear();
        if (descriptor >= 0)
            addDirectoryTree(root);
    }

    [[nodiscard]] std::vector<FileChange> poll() {
        if (descriptor < 0)
            return {};
        // Scripts may not exist when a new project is opened.
        if (watches.empty()) {
            addDirectoryTree(root);
            if (!watches.empty())
                changes.push_back({root, FileChangeType::added});
        }
        bool rescan = false;
        alignas(inotify_event) std::array<std::byte, 16 * 1024> buffer{};
        for (;;) {
            const auto bytes = read(descriptor, buffer.data(), buffer.size());
            if (bytes < 0 && errno == EINTR)
                continue;
            if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            if (bytes <= 0)
                break;
            for (std::size_t offset = 0; offset < static_cast<std::size_t>(bytes);) {
                const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
                if ((event->mask & IN_Q_OVERFLOW) != 0)
                    rescan = true;
                const auto it = watches.find(event->wd);
                if ((event->mask & IN_IGNORED) != 0) {
                    watches.erase(event->wd);
                    offset += sizeof(inotify_event) + event->len;
                    continue;
                } else if (it != watches.end() && (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
                    rescan = true;
                }
                if (it != watches.end() && event->len > 0) {
                    const auto path = it->second / event->name;
                    if ((event->mask & IN_ISDIR) != 0) {
                        // Renamed watches retain their old paths; rebuild the
                        // tree and notify even when a whole folder disappears.
                        rescan = true;
                    }
                    const auto extension = path.extension().string();
                    if (extension == ".cpp" || extension == ".h" || extension == ".hpp" || extension == ".cc" ||
                        extension == ".cxx" || extension == ".slang") {
                        changes.push_back({path, changeType(event->mask)});
                    }
                }
                offset += sizeof(inotify_event) + event->len;
            }
        }
        if (rescan) {
            watch(root);
            changes.push_back({root, FileChangeType::modified});
        }
        return std::exchange(changes, {});
    }

  private:
    static constexpr std::uint32_t flags = IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                                           IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF;

    static FileChangeType changeType(const std::uint32_t mask) {
        if ((mask & IN_CREATE) != 0)
            return FileChangeType::added;
        if ((mask & IN_DELETE) != 0)
            return FileChangeType::removed;
        if ((mask & (IN_MOVED_FROM | IN_MOVED_TO)) != 0)
            return FileChangeType::renamed;
        return FileChangeType::modified;
    }

    void addDirectoryTree(const std::filesystem::path& directory) {
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error) || std::filesystem::is_symlink(directory, error))
            return;
        const int watch = inotify_add_watch(descriptor, directory.c_str(), flags);
        if (watch >= 0)
            watches.insert_or_assign(watch, directory);
        for (std::filesystem::recursive_directory_iterator iterator{directory, error}, end; !error && iterator != end;
             iterator.increment(error)) {
            if (iterator->is_directory(error) && !iterator->is_symlink(error)) {
                const int childWatch = inotify_add_watch(descriptor, iterator->path().c_str(), flags);
                if (childWatch >= 0)
                    watches.insert_or_assign(childWatch, iterator->path());
            }
        }
    }

    int descriptor{-1};
    std::filesystem::path root;
    std::unordered_map<int, std::filesystem::path> watches;
    std::vector<FileChange> changes;
};

FileWatcher::FileWatcher(std::filesystem::path root) : impl_{std::make_unique<Impl>(std::move(root))} {}
FileWatcher::~FileWatcher() = default;
void FileWatcher::watch(std::filesystem::path root) {
    impl_->watch(std::move(root));
}
std::vector<FileChange> FileWatcher::poll() {
    return impl_->poll();
}
} // namespace Platform
