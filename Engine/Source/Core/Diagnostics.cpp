#include "Engine/Core/Diagnostics.h"

#include <chrono>
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace Engine {
    namespace {
        std::tm localTime(const std::time_t value) {
            std::tm result{};
#ifdef _WIN32
            localtime_s(&result, &value);
#else
            localtime_r(&value, &result);
#endif
            return result;
        }

        std::string formatTime(const std::chrono::system_clock::time_point time, const char* format) {
            const auto value = std::chrono::system_clock::to_time_t(time);
            const auto local = localTime(value);
            std::ostringstream stream;
            stream << std::put_time(&local, format);
            return stream.str();
        }

        unsigned long processId() {
#ifdef _WIN32
            return GetCurrentProcessId();
#else
            return static_cast<unsigned long>(getpid());
#endif
        }

        const char* severityName(const DiagnosticSeverity severity) {
            switch (severity) {
            case DiagnosticSeverity::Info: return "Info";
            case DiagnosticSeverity::Warning: return "Warning";
            case DiagnosticSeverity::Error: return "Error";
            }
            return "Unknown";
        }

        void appendContext(std::ostream& output, const DiagnosticContext& context) {
            const auto append = [&](const char* name, const std::string& value) {
                if (!value.empty()) output << " | " << name << ": " << value;
            };
            append("Subsystem", context.subsystem);
            append("Object", context.object);
            append("Component", context.component);
            append("File", context.file);
            append("Suggested action", context.suggestedAction);
        }

        void cleanupOldLogs(const std::filesystem::path& directory) noexcept {
            try {
                struct LogFile final {
                    std::filesystem::path path;
                    std::filesystem::file_time_type modified;
                    std::uintmax_t size;
                };
                std::vector<LogFile> files;
                for (std::filesystem::recursive_directory_iterator iterator{directory}, end;
                     iterator != end; ++iterator) {
                    std::error_code error;
                    if (!iterator->is_regular_file(error) || iterator->path().extension() != ".log") continue;
                    const auto modified = iterator->last_write_time(error);
                    if (error) continue;
                    const auto size = iterator->file_size(error);
                    if (error) continue;
                    files.push_back({iterator->path(), modified, size});
                }
                std::ranges::sort(files, {}, &LogFile::modified);
                constexpr auto keepDays = std::chrono::days{14};
                constexpr std::size_t maxSessions = 50;
                constexpr std::uintmax_t maxTotalSize = 200ULL * 1024ULL * 1024ULL;
                std::uintmax_t totalSize = 0;
                for (const auto& file : files) totalSize += file.size;
                const auto expiry = std::filesystem::file_time_type::clock::now() - keepDays;
                while (!files.empty() && (files.front().modified < expiry || files.size() > maxSessions ||
                                          totalSize > maxTotalSize)) {
                    std::error_code error;
                    std::filesystem::remove(files.front().path, error);
                    if (!error) totalSize -= files.front().size;
                    files.erase(files.begin());
                }
            } catch (...) {
                // Retention must not prevent the Editor from starting.
            }
        }
    }

    Diagnostics &Diagnostics::instance() {
        static Diagnostics diagnostics;
        return diagnostics;
    }

    void Diagnostics::initialize(const std::filesystem::path& logDirectory) noexcept {
        try {
            std::scoped_lock lock{mutex_};
            if (logFile_.is_open()) return;
            logFile_.clear();
            const auto now = std::chrono::system_clock::now();
            cleanupOldLogs(logDirectory);
            const auto directory = logDirectory / formatTime(now, "%Y-%m-%d");
            std::filesystem::create_directories(directory);
            currentLogPath_ = directory / ("Editor_" + formatTime(now, "%Y-%m-%d_%H-%M-%S") +
                                           "_" + std::to_string(processId()) + ".log");
            logFile_.open(currentLogPath_, std::ios::out | std::ios::app);
            if (!logFile_) {
                currentLogPath_.clear();
                return;
            }
            logFile_ << "============================================================\n"
                     << "GamEngine Editor Session\n"
                     << "============================================================\n"
                     << "Started: " << formatTime(now, "%Y-%m-%d %H:%M:%S") << '\n'
                     << "PID: " << processId() << "\n============================================================\n";
        } catch (...) {
            currentLogPath_.clear();
        }
    }

    void Diagnostics::shutdown() noexcept {
        try {
            std::scoped_lock lock{mutex_};
            if (logFile_.is_open()) {
                logFile_ << "============================================================\nSession ended\n";
                logFile_.flush();
                logFile_.close();
            }
        } catch (...) {
        }
    }

    void Diagnostics::report(const DiagnosticSeverity severity, std::string message,
                             DiagnosticContext context) noexcept {
        try {
            std::scoped_lock lock{mutex_};
            constexpr std::size_t maximumEntries = 2'000;
            Diagnostic entry{.sequence = nextSequence_++, .timestamp = std::chrono::system_clock::now(),
                             .threadId = std::this_thread::get_id(), .severity = severity,
                             .message = std::move(message), .context = std::move(context)};
            if (entries_.size() == maximumEntries) entries_.pop_front();
            entries_.push_back(entry);
            if (logFile_.is_open()) {
                logFile_ << formatTime(entry.timestamp, "%H:%M:%S") << " [" << std::left << std::setw(7)
                         << severityName(entry.severity) << "] [" << entry.context.subsystem << "] "
                         << entry.message;
                appendContext(logFile_, entry.context);
                logFile_ << '\n';
                if (severity == DiagnosticSeverity::Error) logFile_.flush();
            }
        } catch (...) {
            // Reporting must never turn a recoverable runtime problem into a crash.
        }
    }

    std::vector<Diagnostic> Diagnostics::entries() const {
        std::scoped_lock lock{mutex_};
        return {entries_.begin(), entries_.end()};
    }

    std::filesystem::path Diagnostics::currentLogPath() const {
        std::scoped_lock lock{mutex_};
        return currentLogPath_;
    }

    void Diagnostics::clear() noexcept {
        std::scoped_lock lock{mutex_};
        entries_.clear();
    }

    void Diagnostics::flush() noexcept {
        try {
            std::scoped_lock lock{mutex_};
            if (logFile_.is_open()) logFile_.flush();
        } catch (...) {
        }
    }
} // namespace Engine
