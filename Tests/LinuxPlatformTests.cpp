#include "Platform/FileWatcher.h"
#include "Platform/Terminal/TerminalSession.h"
#include "Platform/UserPaths.h"
#include "Engine/Scripting/DynamicLibrary.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <thread>

namespace {
class Environment final {
public:
    Environment(const char* name, const std::string& value) : name_(name) {
        if (const char* previous = std::getenv(name)) previous_ = previous;
        ::setenv(name, value.c_str(), 1);
    }
    ~Environment() {
        if (previous_) ::setenv(name_.c_str(), previous_->c_str(), 1);
        else ::unsetenv(name_.c_str());
    }
private:
    std::string name_;
    std::optional<std::string> previous_;
};

class LinuxPlatformTest : public testing::Test {
protected:
    std::filesystem::path root;
    void SetUp() override {
        root = std::filesystem::temp_directory_path() / ("ge-platform-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

TEST_F(LinuxPlatformTest, HonorsAbsoluteXdgDirectoriesAndLocalizedDocuments) {
    const Environment home{"HOME", root.string()};
    const Environment data{"XDG_DATA_HOME", (root / "data").string()};
    const Environment config{"XDG_CONFIG_HOME", (root / "config").string()};
    std::filesystem::create_directories(root / "config");
    std::ofstream{root / "config/user-dirs.dirs"} << "XDG_DOCUMENTS_DIR=\"$HOME/Dokumenty\"\n";
    EXPECT_EQ(Platform::UserPaths::localData(), root / "data");
    EXPECT_EQ(Platform::UserPaths::documents(), root / "Dokumenty");
    const Environment relativeData{"XDG_DATA_HOME", "relative/path"};
    EXPECT_EQ(Platform::UserPaths::localData(), root / ".local/share");
}

TEST_F(LinuxPlatformTest, WatchesNewRenamedAndRemovedDirectoryTrees) {
    std::filesystem::create_directories(root / "scripts/old");
    Platform::FileWatcher watcher{root / "scripts"};
    std::filesystem::rename(root / "scripts/old", root / "scripts/new");
    EXPECT_FALSE(watcher.poll().empty());
    std::ofstream{root / "scripts/new/Test.cpp"} << "// script";
    const auto changes = watcher.poll();
    EXPECT_TRUE(std::ranges::any_of(changes, [&](const auto& change) {
        return change.path == root / "scripts/new/Test.cpp";
    }));
    std::filesystem::remove_all(root / "scripts/new");
    EXPECT_FALSE(watcher.poll().empty());
    std::filesystem::create_directories(root / "scripts/nested/deeper");
    EXPECT_FALSE(watcher.poll().empty());
    std::ofstream{root / "scripts/nested/deeper/Test.hpp"} << "// script";
    EXPECT_FALSE(watcher.poll().empty());
}

TEST_F(LinuxPlatformTest, WatchesInitiallyMissingAndRecreatedRoots) {
    Platform::FileWatcher watcher{root / "scripts"};
    EXPECT_TRUE(watcher.poll().empty());
    std::filesystem::create_directories(root / "scripts");
    EXPECT_FALSE(watcher.poll().empty());
    std::filesystem::remove_all(root / "scripts");
    EXPECT_FALSE(watcher.poll().empty());
    std::filesystem::create_directories(root / "scripts");
    EXPECT_FALSE(watcher.poll().empty());
    std::ofstream{root / "scripts/Test.cpp"} << "// script";
    EXPECT_FALSE(watcher.poll().empty());
}

TEST_F(LinuxPlatformTest, RunsInteractiveShellAndReapsIt) {
    auto terminal = Platform::createTerminalSession();
    ASSERT_TRUE(terminal->start("/bin/sh", root));
    ASSERT_TRUE(terminal->running());
    terminal->resize(80, 24);
    terminal->write("printf 'platform-%s\\n' 'ok'; pwd; exit\n");
    std::string output;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline) {
        output += terminal->readAvailable();
        if (!terminal->running()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    output += terminal->readAvailable();
    EXPECT_NE(output.find("platform-ok"), std::string::npos);
    EXPECT_NE(output.find(root.string()), std::string::npos);
    EXPECT_FALSE(terminal->running());
    terminal->stop();
    EXPECT_FALSE(terminal->start("/does/not/exist", root));
    EXPECT_FALSE(terminal->start("/bin/sh", root / "missing"));
}

TEST(LinuxDynamicLibraryTest, LoadsBareFilenameFromCurrentDirectory) {
    Engine::DynamicLibrary library;
    ASSERT_TRUE(library.load("libLinuxTestModule.so"));
    const auto function = library.symbol<int (*)()>("platformTestValue");
    ASSERT_NE(function, nullptr);
    EXPECT_EQ(function(), 42);
    library.unload();
    EXPECT_FALSE(library.loaded());
}
} // namespace
