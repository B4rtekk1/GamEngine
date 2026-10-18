#include "Platform/Terminal/TerminalSession.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {

class PtyTerminal final : public Platform::TerminalSession {
  public:
    ~PtyTerminal() override {
        stop();
    }

    bool start(const std::filesystem::path& executable, const std::filesystem::path& workingDirectory) override {
        stop();
        const std::string program = executable.string();
        const std::string directory = workingDirectory.string();
        if (program.empty() || ::access(program.c_str(), X_OK) != 0)
            return false;

        // Prepare all allocations before fork: the editor can have other threads.
        std::vector<std::string> environment;
        for (char** entry = environ; *entry != nullptr; ++entry) {
            const std::string_view value{*entry};
            if (!value.starts_with("TERM=") && !value.starts_with("COLORTERM="))
                environment.emplace_back(value);
        }
        environment.emplace_back("TERM=xterm-256color");
        environment.emplace_back("COLORTERM=truecolor");
        std::vector<char*> environmentPointers;
        for (auto& entry : environment)
            environmentPointers.push_back(entry.data());
        environmentPointers.push_back(nullptr);
        char interactive[] = "-i";
        char* arguments[] = {const_cast<char*>(program.c_str()), interactive, nullptr};

        int errors[2];
        if (::pipe2(errors, O_CLOEXEC) != 0)
            return false;
        winsize size{};
        size.ws_col = 120;
        size.ws_row = 30;
        child_ = ::forkpty(&master_, nullptr, nullptr, &size);
        if (child_ == 0) {
            ::close(errors[0]);
            if (directory.empty() || ::chdir(directory.c_str()) == 0)
                ::execve(program.c_str(), arguments, environmentPointers.data());
            const int error = errno;
            static_cast<void>(::write(errors[1], &error, sizeof(error)));
            ::_exit(127);
        }
        ::close(errors[1]);
        if (child_ < 0) {
            ::close(errors[0]);
            stop();
            return false;
        }
        int error{};
        ssize_t count;
        do {
            count = ::read(errors[0], &error, sizeof(error));
        } while (count < 0 && errno == EINTR);
        ::close(errors[0]);
        if (count != 0 || ::fcntl(master_, F_SETFL, O_NONBLOCK) < 0 || ::fcntl(master_, F_SETFD, FD_CLOEXEC) < 0) {
            stop();
            return false;
        }
        return true;
    }

    void stop() override {
        if (master_ >= 0) {
            ::close(master_); // Hang up the controlling terminal, including its foreground job.
            master_ = -1;
        }
        pendingInput_.clear();
        if (child_ <= 0)
            return;
        if (!running())
            return;
        static_cast<void>(::kill(-child_, SIGHUP));
        static_cast<void>(::kill(child_, SIGHUP));
        for (int attempt = 0; attempt < 20 && running(); ++attempt)
            static_cast<void>(::poll(nullptr, 0, 10));
        if (child_ > 0) {
            static_cast<void>(::kill(-child_, SIGKILL));
            static_cast<void>(::kill(child_, SIGKILL));
            while (::waitpid(child_, nullptr, 0) < 0 && errno == EINTR) {
            }
            child_ = -1;
        }
    }

    void write(std::string_view data) override {
        if (master_ < 0)
            return;
        pendingInput_.append(data);
        flushInput();
    }

    [[nodiscard]] std::string readAvailable() override {
        if (master_ < 0)
            return {};
        flushInput();
        std::string output;
        std::array<char, 4096> buffer{};
        // Bound work per frame even when a command continuously produces output.
        while (output.size() < 65536) {
            const ssize_t count = ::read(master_, buffer.data(), buffer.size());
            if (count > 0)
                output.append(buffer.data(), static_cast<size_t>(count));
            else if (count < 0 && errno == EINTR)
                continue;
            else
                break;
        }
        return output;
    }

    void resize(std::uint16_t columns, std::uint16_t rows) override {
        if (master_ < 0)
            return;
        winsize size{};
        size.ws_col = columns;
        size.ws_row = rows;
        static_cast<void>(::ioctl(master_, TIOCSWINSZ, &size));
    }

    [[nodiscard]] bool running() const override {
        if (child_ <= 0)
            return false;
        pid_t result;
        do {
            result = ::waitpid(child_, nullptr, WNOHANG);
        } while (result < 0 && errno == EINTR);
        if (result == 0)
            return true;
        if (result == child_ || (result < 0 && errno == ECHILD))
            child_ = -1;
        return false;
    }

  private:
    void flushInput() {
        while (!pendingInput_.empty()) {
            const ssize_t count = ::write(master_, pendingInput_.data(), pendingInput_.size());
            if (count > 0)
                pendingInput_.erase(0, static_cast<size_t>(count));
            else if (count < 0 && errno == EINTR)
                continue;
            else
                break;
        }
    }

    int master_{-1};
    mutable pid_t child_{-1};
    std::string pendingInput_;
};

} // namespace

namespace Platform {

std::unique_ptr<TerminalSession> createTerminalSession() {
    return std::make_unique<PtyTerminal>();
}

std::filesystem::path defaultTerminalShell() {
    if (const char* shell = std::getenv("SHELL"); shell && ::access(shell, X_OK) == 0)
        return shell;
    return ::access("/bin/bash", X_OK) == 0 ? "/bin/bash" : "/bin/sh";
}

} // namespace Platform
