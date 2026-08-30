#pragma once

#include <functional>
#include <mutex>
#include <string>
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
        DiagnosticSeverity severity;
        std::string message;
        DiagnosticContext context;
    };

    /** Thread-safe, bounded diagnostic stream shared by engine and editor. */
    class Diagnostics final {
    public:
        [[nodiscard]] static Diagnostics &instance();

        void report(DiagnosticSeverity severity, std::string message,
                    DiagnosticContext context = {}) noexcept;
        [[nodiscard]] std::vector<Diagnostic> entries() const;
        void clear() noexcept;

    private:
        mutable std::mutex mutex_;
        std::vector<Diagnostic> entries_;
    };
} // namespace Engine
