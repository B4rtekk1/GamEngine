#pragma once

#include <string>
#include <string_view>

namespace Editor {

enum class LogLevel { Info, Warning, Error };

/** Dockable view over the engine-wide diagnostic stream. */
class ConsolePanel final {
  public:
    static void info(std::string_view message);
    static void warning(std::string_view message);
    static void error(std::string_view message);

    /** Draws the console. @p isOpen is controlled by View > Console. */
    static void draw(bool& isOpen);
    static void clear();

    /** True while the console window (or its children) has keyboard focus. */
    [[nodiscard]] static bool hasFocus();

  private:
    static void add(LogLevel level, std::string_view message);
};

} // namespace Editor
