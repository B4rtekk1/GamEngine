#pragma once

#include <filesystem>
#include <memory>
#include <vector>

namespace Platform {
    enum class FileChangeType { added, modified, removed, renamed };

    struct FileChange final {
        std::filesystem::path path;
        FileChangeType type;
    };

    // Non-blocking, recursive directory watcher. Platform implementations
    // collect native notifications; callers drain them with poll().
    class FileWatcher final {
    public:
        explicit FileWatcher(std::filesystem::path root);
        ~FileWatcher();

        FileWatcher(const FileWatcher&) = delete;
        FileWatcher& operator=(const FileWatcher&) = delete;

        void watch(std::filesystem::path root);
        [[nodiscard]] std::vector<FileChange> poll();

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace Platform
