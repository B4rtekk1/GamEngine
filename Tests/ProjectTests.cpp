#include <gtest/gtest.h>

#include "Engine/Project.h"

#include <chrono>
#include <fstream>

namespace {

class ProjectTest : public testing::Test {
protected:
    void SetUp() override {
        directory_ = std::filesystem::temp_directory_path() /
                     ("gameengine-project-" + std::to_string(
                         std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory_ / "Assets/Scenes");
        std::ofstream output{directory_ / "GamEngine.project"};
        output << "name = Test Game\nasset_root = Assets\nstartup_scene = Assets/Scenes/Main.scene\n";
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    std::filesystem::path directory_;
};

TEST_F(ProjectTest, ResolvesAssetAndStartupPathsFromManifestDirectory) {
    const Engine::Project project = Engine::Project::load(directory_ / "GamEngine.project");
    EXPECT_EQ(project.name(), "Test Game");
    EXPECT_EQ(project.assetRoot(), directory_ / "Assets");
    EXPECT_EQ(project.startupScene(), directory_ / "Assets/Scenes/Main.scene");
    EXPECT_EQ(project.resolve("Assets/Models/cube.glb"), directory_ / "Assets/Models/cube.glb");
}

TEST_F(ProjectTest, FindsProjectFromNestedDirectory) {
    const Engine::Project project = Engine::Project::discover(directory_ / "Assets/Scenes");
    EXPECT_EQ(project.manifestPath(), directory_ / "GamEngine.project");
}

TEST(Project, UsesBuiltInPathsWithoutManifest) {
    const auto directory = std::filesystem::temp_directory_path() / "gameengine-default-project";
    const Engine::Project project = Engine::Project::defaults(directory);
    EXPECT_TRUE(project.manifestPath().empty());
    EXPECT_EQ(project.assetRoot(), directory / "Assets");
    EXPECT_EQ(project.startupScene(), directory / "Assets/Scenes/Editor.scene");
}

TEST(Project, RejectsPathOutsideProject) {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("gameengine-invalid-project-" + std::to_string(
                               std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    {
        std::ofstream output{directory / "GamEngine.project"};
        output << "asset_root = ../Assets\nstartup_scene = Assets/Scenes/Main.scene\n";
    }
    EXPECT_THROW(static_cast<void>(Engine::Project::load(directory / "GamEngine.project")),
                 std::runtime_error);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

} // namespace
