#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace Platform {

class TerminalSession {
public:
    virtual ~TerminalSession() = default;

    virtual bool start(const std::filesystem::path& executable,
                       const std::filesystem::path& workingDirectory) = 0;
    /** Stops the native session and releases all of its background work. */
    virtual void stop() = 0;
    virtual void write(std::string_view data) = 0;
    [[nodiscard]] virtual std::string readAvailable() = 0;
    virtual void resize(std::uint16_t columns, std::uint16_t rows) = 0;
    [[nodiscard]] virtual bool running() const = 0;
};

/** Creates the native terminal backend for the current platform. */
[[nodiscard]] std::unique_ptr<TerminalSession> createTerminalSession();
/** Returns pwsh, Windows PowerShell, or cmd in that order when available. */
[[nodiscard]] std::filesystem::path defaultTerminalShell();

} // namespace Platform
