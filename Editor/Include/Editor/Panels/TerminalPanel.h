#pragma once

#include "Platform/Terminal/TerminalBuffer.h"

#include <filesystem>
#include <memory>

namespace Platform { class TerminalSession; }
struct ImFont;

namespace Editor {

/** Dockable interactive shell, kept separate from the engine diagnostics console. */
class TerminalPanel final {
public:
    TerminalPanel(std::filesystem::path projectRoot, ImFont* terminalFont);
    ~TerminalPanel();

    TerminalPanel(const TerminalPanel&) = delete;
    TerminalPanel& operator=(const TerminalPanel&) = delete;

    /** Stops the terminal before graphics and window services are torn down. */
    void shutdown();
    void draw(bool& isOpen);

private:
    void restart();
    void handleKeyboard();
    void drawScreen();

    std::filesystem::path projectRoot_;
    std::filesystem::path shell_;
    std::unique_ptr<Platform::TerminalSession> session_;
    Platform::TerminalBuffer buffer_;
    float cellWidth_{};
    float cellHeight_{};
    bool hasKeyboardFocus_{};
    bool startFailed_{};
    ImFont* terminalFont_{};
};

} // namespace Editor
