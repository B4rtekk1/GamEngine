#include "Engine/Scripting/Console.h"

#include "ConsoleSource.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Engine {
namespace {
thread_local const ConsoleSource* currentSource = nullptr;
thread_local int groupDepth = 0;

// Looks labels up by string_view without allocating a key.
struct LabelHash final {
    using is_transparent = void;
    std::size_t operator()(const std::string_view label) const noexcept {
        return std::hash<std::string_view>{}(label);
    }
};

// Labels are shared by every script, as in a browser.
struct LabelState final {
    std::mutex mutex;
    std::unordered_map<std::string, std::uint64_t, LabelHash, std::equal_to<>> counts;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point, LabelHash, std::equal_to<>> timers;
};

LabelState& labels() {
    static LabelState state;
    return state;
}

std::string elapsedText(const std::string_view label, const std::chrono::steady_clock::time_point start) {
    const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - start;
    return std::format("{}: {:.3f} ms", label, elapsed.count());
}
} // namespace

ScopedConsoleSource::ScopedConsoleSource(const ConsoleSource& source) noexcept
    : previous_(currentSource), previousGroupDepth_(groupDepth) {
    currentSource = &source;
}

ScopedConsoleSource::~ScopedConsoleSource() {
    currentSource = previous_;
    groupDepth = previousGroupDepth_;
}

void Console::write(const DiagnosticSeverity severity, std::string message) {
    DiagnosticContext context;
    context.subsystem = "Script";
    if (currentSource != nullptr && currentSource->context != nullptr)
        context = currentSource->context(currentSource->owner);
    if (groupDepth > 0)
        message.insert(0, static_cast<std::size_t>(groupDepth) * 2, ' ');
    Diagnostics::instance().report(severity, std::move(message), std::move(context));
}

void Console::count(const std::string_view label) const {
    std::uint64_t value = 0;
    {
        LabelState& state = labels();
        const std::scoped_lock lock{state.mutex};
        auto found = state.counts.find(label);
        if (found == state.counts.end())
            found = state.counts.emplace(std::string{label}, 0).first;
        value = ++found->second;
    }
    write(DiagnosticSeverity::Info, std::format("{}: {}", label, value));
}

void Console::countReset(const std::string_view label) const {
    LabelState& state = labels();
    const std::scoped_lock lock{state.mutex};
    if (const auto found = state.counts.find(label); found != state.counts.end())
        found->second = 0;
}

void Console::time(const std::string_view label) const {
    bool exists = false;
    {
        LabelState& state = labels();
        const std::scoped_lock lock{state.mutex};
        exists = !state.timers.try_emplace(std::string{label}, std::chrono::steady_clock::now()).second;
    }
    if (exists)
        write(DiagnosticSeverity::Warning, std::format("Timer '{}' already exists", label));
}

void Console::timeLog(const std::string_view label) const {
    std::chrono::steady_clock::time_point start;
    bool found = false;
    {
        LabelState& state = labels();
        const std::scoped_lock lock{state.mutex};
        if (const auto timer = state.timers.find(label); timer != state.timers.end()) {
            start = timer->second;
            found = true;
        }
    }
    if (found)
        write(DiagnosticSeverity::Info, elapsedText(label, start));
    else
        write(DiagnosticSeverity::Warning, std::format("Timer '{}' does not exist", label));
}

void Console::timeEnd(const std::string_view label) const {
    std::chrono::steady_clock::time_point start;
    bool found = false;
    {
        LabelState& state = labels();
        const std::scoped_lock lock{state.mutex};
        if (const auto timer = state.timers.find(label); timer != state.timers.end()) {
            start = timer->second;
            state.timers.erase(timer);
            found = true;
        }
    }
    if (found)
        write(DiagnosticSeverity::Info, elapsedText(label, start));
    else
        write(DiagnosticSeverity::Warning, std::format("Timer '{}' does not exist", label));
}

void Console::group(const std::string_view label) const {
    if (!label.empty())
        write(DiagnosticSeverity::Info, std::string{label});
    ++groupDepth;
}

void Console::groupEnd() const {
    if (groupDepth > 0)
        --groupDepth;
}

void Console::clear() const {
    Diagnostics::instance().clear();
}
} // namespace Engine
