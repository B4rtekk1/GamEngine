#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace Engine {
    /** Severity used by the engine-wide diagnostic stream. */
    enum class DiagnosticSeverity { Info, Warning, Error };

    /** Context that lets an editor identify both the failure and its owner. */
    struct DiagnosticContext final {
        std::string subsystem;
        std::string object;
        std::string component;
        std::string file;
        std::string suggestedAction;
    };

    struct Diagnostic final {
        std::uint64_t sequence = 0;
        std::chrono::system_clock::time_point timestamp;
        std::thread::id threadId;
        DiagnosticSeverity severity;
        std::string message;
        DiagnosticContext context;
    };

    /** Thread-safe, bounded diagnostic stream shared by engine and editor. */
    class Diagnostics final {
    public:
        [[nodiscard]] static Diagnostics &instance();

        /** Starts a file sink for this process. Safe to call again after shutdown. */
        void initialize(const std::filesystem::path& logDirectory) noexcept;
        void shutdown() noexcept;
        void report(DiagnosticSeverity severity, std::string message,
                    DiagnosticContext context = {}) noexcept;
        [[nodiscard]] std::vector<Diagnostic> entries() const;
        [[nodiscard]] std::filesystem::path currentLogPath() const;
        void clear() noexcept;
        void flush() noexcept;

    private:
        mutable std::mutex mutex_;
        std::deque<Diagnostic> entries_;
        std::ofstream logFile_;
        std::filesystem::path currentLogPath_;
        std::uint64_t nextSequence_ = 1;
    };
} // namespace Engine
