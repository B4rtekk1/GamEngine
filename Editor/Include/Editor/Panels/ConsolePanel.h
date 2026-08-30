#pragma once

#include <string>
#include <string_view>

namespace Editor {

enum class LogLevel { Info, Warning, Error };

/** Thread-safe in-editor log buffer and its dockable ImGui panel. */
class ConsolePanel final {
public:
    static void info(std::string_view message);
    static void warning(std::string_view message);
    static void error(std::string_view message);

    /** Draws the console. @p isOpen is controlled by View > Console. */
    static void draw(bool& isOpen);
    static void clear();

private:
    static void add(LogLevel level, std::string_view message);
};

} // namespace Editor
