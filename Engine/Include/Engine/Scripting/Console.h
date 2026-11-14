#pragma once

#include "Engine/Core/Diagnostics.h"
#include "Engine/Math/Quat.h"
#include "Engine/Math/Vec2.h"
#include "Engine/Math/Vec3.h"

#include <concepts>
#include <cstddef>
#include <format>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

template <> struct std::formatter<Engine::Vec2> : std::formatter<float> {
    // Generic over the context, as std::formattable checks a non-format_context one.
    template <typename FormatContext> auto format(const Engine::Vec2& value, FormatContext& context) const {
        auto out = std::format_to(context.out(), "(");
        context.advance_to(out);
        out = std::formatter<float>::format(value.x(), context);
        out = std::format_to(out, ", ");
        context.advance_to(out);
        out = std::formatter<float>::format(value.y(), context);
        return std::format_to(out, ")");
    }
};

template <> struct std::formatter<Engine::Vec3> : std::formatter<float> {
    // Generic over the context, as std::formattable checks a non-format_context one.
    template <typename FormatContext> auto format(const Engine::Vec3& value, FormatContext& context) const {
        auto out = std::format_to(context.out(), "(");
        for (int axis = 0; axis < 3; ++axis) {
            if (axis != 0)
                out = std::format_to(out, ", ");
            context.advance_to(out);
            out = std::formatter<float>::format(axis == 0 ? value.x() : axis == 1 ? value.y() : value.z(), context);
        }
        return std::format_to(out, ")");
    }
};

template <> struct std::formatter<Engine::Quat> : std::formatter<float> {
    // Generic over the context, as std::formattable checks a non-format_context one.
    template <typename FormatContext> auto format(const Engine::Quat& value, FormatContext& context) const {
        auto out = std::format_to(context.out(), "(");
        const float components[] = {value.x(), value.y(), value.z(), value.w()};
        for (int index = 0; index < 4; ++index) {
            if (index != 0)
                out = std::format_to(out, ", ");
            context.advance_to(out);
            out = std::formatter<float>::format(components[index], context);
        }
        return std::format_to(out, ")");
    }
};

namespace Engine {

namespace ConsoleDetail {
template <typename T>
concept Streamable = requires(std::ostream& stream, const T& value) { stream << value; };

[[nodiscard]] constexpr bool hasReplacementField(const std::string_view text) {
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '{')
            continue;
        if (index + 1 < text.size() && text[index + 1] == '{') {
            ++index;
            continue;
        }
        return true;
    }
    return false;
}

/**
 * The leading string literal of a Console call. With a "{}" field it is a
 * std::format string, checked against the arguments at compile time;
 * without one the arguments are appended after it, separated by spaces.
 */
template <typename... Args> struct Message final {
    std::string_view text;
    bool formatted{};

    template <std::size_t Size>
    consteval Message(const char (&literal)[Size]) // NOLINT(google-explicit-constructor)
        : text(literal, Size - 1), formatted(sizeof...(Args) > 0 && hasReplacementField(text)) {
        if (formatted)
            static_cast<void>(std::format_string<Args...>(literal));
    }
};

// String literals bind to Message, so they never take the plain-value path.
template <typename T>
concept PlainValue = !std::is_array_v<std::remove_cvref_t<T>>;

template <typename T> [[nodiscard]] std::string text(const T& value) {
    if constexpr (std::convertible_to<const T&, std::string_view>)
        return std::string{std::string_view{value}};
    else if constexpr (std::formattable<T, char>)
        return std::format("{}", value);
    else {
        static_assert(Streamable<T>, "Console: the value is neither std::formattable nor streamable");
        std::ostringstream stream;
        stream << value;
        return std::move(stream).str();
    }
}

template <typename First, typename... Rest> [[nodiscard]] std::string join(const First& first, const Rest&... rest) {
    std::string result = text(first);
    ((result += ' ', result += text(rest)), ...);
    return result;
}

template <typename... Args> [[nodiscard]] std::string compose(const Message<Args...>& message, const Args&... args) {
    if (message.formatted)
        return std::vformat(message.text, std::make_format_args(args...));
    return join(message.text, args...);
}
} // namespace ConsoleDetail

/**
 * Script log in the style of the browser console, shown in the editor
 * Console and written to the log file.
 *
 *     console.log("Jumped");
 *     console.log("Health:", health, "at", transform().position); // space-separated
 *     console.warn("Low health: {}", health);                     // std::format syntax
 *     console.error(target);                                      // any formattable value
 *     console.assertThat(health >= 0, "Negative health {}", health);
 *
 *     console.time("path");  ...  console.timeEnd("path");        // "path: 0.42 ms"
 *     console.count("hit");                                       // "hit: 3"
 *     console.group("Spawn"); ... console.groupEnd();             // indents what is between
 *
 * Inside a Script `console` is a member; elsewhere use Engine::console.
 * A message logged from a script callback names the scripted object and
 * script class in the Console, so it can be filtered by either.
 */
class Console final {
  public:
    template <typename... Args>
    void log(ConsoleDetail::Message<std::type_identity_t<Args>...> message, const Args&... args) const {
        write(DiagnosticSeverity::Info, ConsoleDetail::compose(message, args...));
    }
    template <ConsoleDetail::PlainValue First, typename... Rest>
    void log(const First& first, const Rest&... rest) const {
        write(DiagnosticSeverity::Info, ConsoleDetail::join(first, rest...));
    }

    /** Same as log(). */
    template <typename... Args>
    void info(ConsoleDetail::Message<std::type_identity_t<Args>...> message, const Args&... args) const {
        write(DiagnosticSeverity::Info, ConsoleDetail::compose(message, args...));
    }
    template <ConsoleDetail::PlainValue First, typename... Rest>
    void info(const First& first, const Rest&... rest) const {
        write(DiagnosticSeverity::Info, ConsoleDetail::join(first, rest...));
    }

    /** Info-level message tagged "[debug]", so it can be filtered out. */
    template <typename... Args>
    void debug(ConsoleDetail::Message<std::type_identity_t<Args>...> message, const Args&... args) const {
        write(DiagnosticSeverity::Info, "[debug] " + ConsoleDetail::compose(message, args...));
    }
    template <ConsoleDetail::PlainValue First, typename... Rest>
    void debug(const First& first, const Rest&... rest) const {
        write(DiagnosticSeverity::Info, "[debug] " + ConsoleDetail::join(first, rest...));
    }

    template <typename... Args>
    void warn(ConsoleDetail::Message<std::type_identity_t<Args>...> message, const Args&... args) const {
        write(DiagnosticSeverity::Warning, ConsoleDetail::compose(message, args...));
    }
    template <ConsoleDetail::PlainValue First, typename... Rest>
    void warn(const First& first, const Rest&... rest) const {
        write(DiagnosticSeverity::Warning, ConsoleDetail::join(first, rest...));
    }

    template <typename... Args>
    void error(ConsoleDetail::Message<std::type_identity_t<Args>...> message, const Args&... args) const {
        write(DiagnosticSeverity::Error, ConsoleDetail::compose(message, args...));
    }
    template <ConsoleDetail::PlainValue First, typename... Rest>
    void error(const First& first, const Rest&... rest) const {
        write(DiagnosticSeverity::Error, ConsoleDetail::join(first, rest...));
    }

    /**
     * Logs an error when `condition` is false; console.assert in a browser
     * (`assert` itself is the <cassert> macro).
     */
    void assertThat(const bool condition) const {
        if (!condition)
            write(DiagnosticSeverity::Error, "Assertion failed");
    }
    template <typename... Args>
    void assertThat(const bool condition, ConsoleDetail::Message<std::type_identity_t<Args>...> message,
                    const Args&... args) const {
        if (!condition)
            write(DiagnosticSeverity::Error, "Assertion failed: " + ConsoleDetail::compose(message, args...));
    }

    /** Logs how many times count() has been called with this label. */
    void count(std::string_view label = "default") const;
    void countReset(std::string_view label = "default") const;

    /** Starts a timer; timeLog() and timeEnd() print the elapsed milliseconds. */
    void time(std::string_view label = "default") const;
    void timeLog(std::string_view label = "default") const;
    void timeEnd(std::string_view label = "default") const;

    /**
     * Indents later messages until groupEnd(). An unbalanced group ends with
     * the script callback that opened it.
     */
    void group(std::string_view label = {}) const;
    void groupEnd() const;

    /** Clears the editor Console. The log file keeps every message. */
    void clear() const;

    static void write(DiagnosticSeverity severity, std::string message);
};

inline constexpr Console console{};
} // namespace Engine
