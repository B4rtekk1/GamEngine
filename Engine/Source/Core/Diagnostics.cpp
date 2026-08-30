#include "Engine/Core/Diagnostics.h"

#include <utility>

namespace Engine {
    Diagnostics &Diagnostics::instance() {
        static Diagnostics diagnostics;
        return diagnostics;
    }

    void Diagnostics::report(const DiagnosticSeverity severity, std::string message,
                             DiagnosticContext context) noexcept {
        try {
            std::scoped_lock lock{mutex_};
            constexpr std::size_t maximumEntries = 2'000;
            if (entries_.size() == maximumEntries) entries_.erase(entries_.begin());
            entries_.push_back({severity, std::move(message), std::move(context)});
        } catch (...) {
            // Reporting must never turn a recoverable runtime problem into a crash.
        }
    }

    std::vector<Diagnostic> Diagnostics::entries() const {
        std::scoped_lock lock{mutex_};
        return entries_;
    }

    void Diagnostics::clear() noexcept {
        std::scoped_lock lock{mutex_};
        entries_.clear();
    }
} // namespace Engine
