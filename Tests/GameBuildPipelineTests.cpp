#include "Editor/Build/GameBuildPipeline.h"
#include "Engine/Scripting/DynamicLibrary.h"
#include "Engine/Scripting/ScriptModuleApi.h"
#include "Platform/RuntimePaths.h"

#include <SDL3/SDL.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <gtest/gtest.h>

namespace {
TEST(GameBuildPipeline, ExportsScriptsAndRuntimeFromSdkInUnicodeProjectPath) {
    const auto root = std::filesystem::temp_directory_path() / std::filesystem::path{u8"ge gra żółw "} /
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{root};
    const auto project = Engine::Project::create(root / "Project", "SdkTestGame");
    const auto writeScript = [&](int generation) {
        std::ofstream{project.assetRoot() / "Scripts/ProbeBehavior.h"}
            << "#pragma once\n#include <Engine/Scripting/Script.h>\n"
            << "#include <Engine/Scripting/ScriptModuleApi.h>\n"
            << "class ProbeBehavior : public Engine::Script {\npublic:\n"
            << "    void onCreate() override {}\n};\n"
            << "inline int probeGeneration() { static int value = " << generation << "; return value; }\n"
            << "extern \"C\" GAME_SCRIPT_API int GE_TestGeneration() { return probeGeneration(); }\n";
    };
    writeScript(1);
    const auto output = root / "Export";
    const char* overrideRoot = std::getenv("GE_TEST_EDITOR_ROOT_OVERRIDE");
    Editor::GameBuildPipeline pipeline{overrideRoot ? overrideRoot : GE_TEST_EDITOR_ROOT};
    const auto result = pipeline.build(project, {.outputDirectory = output});
    ASSERT_TRUE(result.success) << (result.errors.empty() ? "" : result.errors.front());
    EXPECT_EQ(result.executable, output / (std::string{"SdkTestGame"} + Platform::ExecutableSuffix));
    EXPECT_TRUE(std::filesystem::is_regular_file(output / Platform::EngineLibraryFilename));
    EXPECT_TRUE(std::filesystem::is_regular_file(output / "Fonts/DroidSans.ttf"));
    EXPECT_TRUE(std::filesystem::is_regular_file(output / "Licenses/NOTICE-Fonts.txt"));
    EXPECT_TRUE(std::filesystem::is_directory(output / "shaders"));
    EXPECT_FALSE(std::filesystem::exists(output / "Content/Scripts"));
    const auto exportedProject = Engine::Project::load(output / "GamEngine.project");
    EXPECT_TRUE(std::filesystem::is_regular_file(exportedProject.startupScene()));
    const auto firstGeneration = root / (std::string{"first-"} + Platform::ScriptModuleFilename);
    std::filesystem::copy_file(output / Platform::ScriptModuleFilename, firstGeneration);
    Engine::DynamicLibrary scripts;
    ASSERT_TRUE(scripts.load(firstGeneration));
    const auto version = scripts.symbol<decltype(&GE_GetScriptApiVersion)>("GE_GetScriptApiVersion");
    ASSERT_NE(version, nullptr);
    EXPECT_EQ(version(), ENGINE_SCRIPT_API_VERSION);
    const auto firstValue = scripts.symbol<int (*)()>("GE_TestGeneration");
    ASSERT_NE(firstValue, nullptr);
    EXPECT_EQ(firstValue(), 1);
    // Keep the previous generation mapped while the rebuilt module is loaded,
    // as the editor does when validating a hot-reload candidate.
    writeScript(2);
    const auto rebuilt = pipeline.build(project, {.outputDirectory = output});
    ASSERT_TRUE(rebuilt.success) << (rebuilt.errors.empty() ? "" : rebuilt.errors.front());
    Engine::DynamicLibrary nextScripts;
    ASSERT_TRUE(nextScripts.load(output / Platform::ScriptModuleFilename));
    const auto nextValue = nextScripts.symbol<int (*)()>("GE_TestGeneration");
    ASSERT_NE(nextValue, nullptr);
    EXPECT_EQ(nextValue(), 2);
    EXPECT_EQ(firstValue(), 1);
#ifndef _WIN32
    const auto permissions = std::filesystem::status(result.executable).permissions();
    EXPECT_NE(permissions & std::filesystem::perms::owner_exec, std::filesystem::perms::none);
    EXPECT_TRUE(std::filesystem::is_regular_file(output / "libSDL3.so.0"));
    // Exercise the copied executable's ELF loader without needing a display or
    // GPU: project validation runs before SDL's renderer initialization.
    const std::string executableName = result.executable.string();
    const std::string missingManifest = (root / "missing.project").string();
    const char* arguments[]{executableName.c_str(), "--project", missingManifest.c_str(), nullptr};
    SDL_Environment* environment = SDL_CreateEnvironment(true);
    ASSERT_NE(environment, nullptr);
    SDL_SetEnvironmentVariable(environment, "XDG_DATA_HOME", (root / "data").string().c_str(), true);
    SDL_SetEnvironmentVariable(environment, "LD_LIBRARY_PATH", "", true);
    const auto properties = SDL_CreateProperties();
    SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, arguments);
    SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, environment);
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_Process* process = SDL_CreateProcessWithProperties(properties);
    SDL_DestroyProperties(properties);
    SDL_DestroyEnvironment(environment);
    ASSERT_NE(process, nullptr) << SDL_GetError();
    size_t size{};
    int exitCode = -1;
    void* captured = SDL_ReadProcess(process, &size, &exitCode);
    const std::string message = captured ? std::string{static_cast<const char*>(captured), size} : "";
    SDL_free(captured);
    SDL_DestroyProcess(process);
    EXPECT_EQ(exitCode, EXIT_FAILURE) << message;
    EXPECT_NE(message.find("Could not open project manifest"), std::string::npos) << message;
#endif
}

// The export leaves out sources the game only uses cooked (.gmesh points at
// .gtex), keeps sources a scene loads by path, and rewrites old absolute mesh
// paths so the game runs on another machine.
TEST(GameBuildPipeline, ShipsCookedAssetsAndPortableScenePaths) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("ge-portable-export-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{root};
    const auto project = Engine::Project::create(root / "Project", "PortableGame");
    const auto assets = project.assetRoot();
    std::filesystem::create_directories(assets / "Bistro" / "Textures");
    const auto write = [](const std::filesystem::path& path, const std::string& text) {
        std::ofstream{path, std::ios::binary} << text;
    };
    write(assets / "Bistro/Bistro.fbx", "source model");
    write(assets / "Bistro/Bistro.gmesh", "cooked model");
    write(assets / "Bistro/Textures/Brick.dds", "source texture");
    write(assets / "Bistro/Textures/Brick.bc5.gtex", "cooked texture");
    write(assets / "Bistro/Sky.png", "environment source");
    write(assets / "Bistro/Sky.gtex", "cooked environment");
    write(assets / "Loose.png", "uncooked texture");
    const auto scene = project.startupScene();
    std::ifstream original{scene};
    std::string text{std::istreambuf_iterator<char>{original}, {}};
    original.close();
    // An old scene: absolute mesh path into this project's assets, and a
    // sidecar naming a source texture.
    const std::string absoluteMesh = (assets / "Bistro/Bistro.gmesh").generic_string();
    const auto meshes = text.find("MESHES ");
    ASSERT_NE(meshes, std::string::npos);
    const auto lineEnd = text.find('\n', meshes);
    text.replace(meshes, lineEnd - meshes, "MESHES 1\nMESH_ASSET 0 \"" + absoluteMesh + "\"");
    write(scene, text);
    write(std::filesystem::path{scene.string() + ".environment"}, "\"Bistro/Sky.png\"");

    const auto output = root / "Export";
    const char* overrideRoot = std::getenv("GE_TEST_EDITOR_ROOT_OVERRIDE");
    const Editor::GameBuildPipeline pipeline{overrideRoot ? overrideRoot : GE_TEST_EDITOR_ROOT};
    const auto result = pipeline.build(project, {.outputDirectory = output});
    ASSERT_TRUE(result.success) << (result.errors.empty() ? "" : result.errors.front());
    const auto content = output / "Content";
    EXPECT_TRUE(std::filesystem::exists(content / "Bistro/Bistro.gmesh"));
    EXPECT_FALSE(std::filesystem::exists(content / "Bistro/Bistro.fbx"));
    EXPECT_TRUE(std::filesystem::exists(content / "Bistro/Textures/Brick.bc5.gtex"));
    EXPECT_FALSE(std::filesystem::exists(content / "Bistro/Textures/Brick.dds"));
    EXPECT_TRUE(std::filesystem::exists(content / "Bistro/Sky.png")) << "named by the scene's environment";
    EXPECT_TRUE(std::filesystem::exists(content / "Loose.png")) << "no cooked version";
    const auto exportedScene = content / scene.lexically_relative(assets);
    std::ifstream exported{exportedScene};
    const std::string exportedText{std::istreambuf_iterator<char>{exported}, {}};
    EXPECT_NE(exportedText.find("MESH_ASSET 0 \"Bistro/Bistro.gmesh\""), std::string::npos) << exportedText;
    EXPECT_EQ(exportedText.find(root.generic_string()), std::string::npos);
}

TEST(GameBuildPipeline, StartupSceneOnlyShipsWhatTheSceneUses) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("ge-startup-only-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{root};
    const auto project = Engine::Project::create(root / "Project", "StartupOnly");
    const auto assets = project.assetRoot();
    const auto write = [](const std::filesystem::path& path, const std::string& text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream{path, std::ios::binary} << text;
    };
    write(assets / "City/City.gmesh", "used mesh");
    write(assets / "City/Textures/Road.gtex", "its texture");
    write(assets / "City/Sky.hdr", "used environment");
    write(assets / "Unused/Forest.gmesh", "another scene's mesh");
    write(assets / "Scenes/Other.scene", "another scene");
    const auto scene = project.startupScene();
    std::ifstream original{scene};
    std::string text{std::istreambuf_iterator<char>{original}, {}};
    original.close();
    const auto meshes = text.find("MESHES ");
    ASSERT_NE(meshes, std::string::npos);
    text.replace(meshes, text.find('\n', meshes) - meshes, "MESHES 1\nMESH_ASSET 0 \"City/City.gmesh\"");
    write(scene, text);
    write(std::filesystem::path{scene.string() + ".environment"}, "\"City/Sky.hdr\"");

    const auto output = root / "Export";
    const char* overrideRoot = std::getenv("GE_TEST_EDITOR_ROOT_OVERRIDE");
    const Editor::GameBuildPipeline pipeline{overrideRoot ? overrideRoot : GE_TEST_EDITOR_ROOT};
    const auto result = pipeline.build(project, {.outputDirectory = output, .startupSceneOnly = true});
    ASSERT_TRUE(result.success) << (result.errors.empty() ? "" : result.errors.front());
    const auto content = output / "Content";
    EXPECT_TRUE(std::filesystem::exists(content / scene.lexically_relative(assets)));
    EXPECT_TRUE(std::filesystem::exists(content / "City/City.gmesh"));
    EXPECT_TRUE(std::filesystem::exists(content / "City/Textures/Road.gtex")) << "the mesh's asset folder";
    EXPECT_TRUE(std::filesystem::exists(content / "City/Sky.hdr"));
    EXPECT_FALSE(std::filesystem::exists(content / "Unused"));
    EXPECT_FALSE(std::filesystem::exists(content / "Scenes/Other.scene"));
}
} // namespace
