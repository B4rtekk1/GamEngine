#include "Engine/Core/Transform.h"
#include "Engine/ECS/Components/ScriptComponent.h"
#include "Engine/ECS/Registry.h"
#include "Engine/Plugins/PluginApi.h"
#include "Engine/Plugins/PluginManager.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scripting/ScriptRegistry.h"
#include "Engine/Scripting/ScriptSystem.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

#ifndef GE_TEST_PLUGIN_LIBRARY
#error "GE_TEST_PLUGIN_LIBRARY must name the built TestPlugin library"
#endif

namespace {
// A temporary plugin root with plugin folders built from the fixture library.
class PluginRoot final {
  public:
    PluginRoot() {
        path_ = std::filesystem::temp_directory_path() /
                ("gamengine-plugins-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~PluginRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    PluginRoot(const PluginRoot&) = delete;
    PluginRoot& operator=(const PluginRoot&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    /** Writes <Name>/<Name>.geplugin and, unless @p withLibrary is false, the fixture library. */
    std::filesystem::path add(const std::string& name, const std::string& extraManifest = {},
                              const bool withLibrary = true) const {
        const auto folder = path_ / name;
        std::filesystem::create_directories(folder);
        std::ofstream manifest{folder / (name + ".geplugin")};
        manifest << "name = " << name << "\nversion = 1.0.0\napi_version = " << ENGINE_PLUGIN_API_VERSION << '\n'
                 << extraManifest;
        if (withLibrary) {
            const auto binaries = folder / "Binaries" / Engine::PluginManager::platformDirectory();
            std::filesystem::create_directories(binaries);
            const std::filesystem::path fixture{GE_TEST_PLUGIN_LIBRARY};
#ifdef _WIN32
            const std::string filename = name + ".dll";
#else
            const std::string filename = "lib" + name + ".so";
#endif
            std::filesystem::copy_file(fixture, binaries / filename, std::filesystem::copy_options::overwrite_existing);
        }
        return folder;
    }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] const Engine::PluginStatus* find(const Engine::PluginManager& manager, const std::string& name) {
    const auto& plugins = manager.plugins();
    const auto found =
        std::ranges::find_if(plugins, [&](const auto& status) { return status.descriptor.name == name; });
    return found == plugins.end() ? nullptr : &*found;
}

[[nodiscard]] std::size_t logged(const std::string& text) {
    const auto entries = Engine::Diagnostics::instance().entries();
    return static_cast<std::size_t>(std::ranges::count_if(
        entries, [&](const Engine::Diagnostic& entry) { return entry.message.find(text) != std::string::npos; }));
}
} // namespace

TEST(PluginManager, ParsesManifestDefaultsAndLists) {
    PluginRoot root;
    const auto folder = root.add("Weather", "description = Rain\ndependencies = Wind, Clouds\n", false);
    std::string error;
    const auto descriptor = Engine::PluginManager::parseManifest(folder / "Weather.geplugin", error);
    ASSERT_TRUE(descriptor.has_value()) << error;
    EXPECT_EQ(descriptor->name, "Weather");
    EXPECT_EQ(descriptor->version, "1.0.0");
    EXPECT_EQ(descriptor->type, "runtime");
    EXPECT_EQ(descriptor->libraryName, "Weather");
    EXPECT_EQ(descriptor->description, "Rain");
    EXPECT_EQ(descriptor->dependencies, (std::vector<std::string>{"Wind", "Clouds"}));
}

TEST(PluginManager, RejectsManifestsWithoutNameOrApiVersion) {
    PluginRoot root;
    std::filesystem::create_directories(root.path() / "Broken");
    std::ofstream{root.path() / "Broken" / "Broken.geplugin"} << "name = Broken\n";
    std::string error;
    EXPECT_FALSE(Engine::PluginManager::parseManifest(root.path() / "Broken" / "Broken.geplugin", error));
    EXPECT_NE(error.find("api_version"), std::string::npos);
    std::ofstream{root.path() / "Broken" / "Broken.geplugin"} << "name = bad/name\napi_version = 1\n";
    EXPECT_FALSE(Engine::PluginManager::parseManifest(root.path() / "Broken" / "Broken.geplugin", error));
}

TEST(PluginManager, LaterRootsCannotShadowAPluginName) {
    PluginRoot engineRoot;
    PluginRoot projectRoot;
    engineRoot.add("Shared", {}, false);
    projectRoot.add("Shared", {}, false);
    const auto statuses = Engine::PluginManager::discover({engineRoot.path(), projectRoot.path()});
    ASSERT_EQ(statuses.size(), 2U);
    EXPECT_EQ(statuses[0].state, Engine::PluginState::Discovered);
    EXPECT_EQ(statuses[1].state, Engine::PluginState::Failed);
    EXPECT_NE(statuses[1].error.find("Duplicate"), std::string::npos);
}

TEST(PluginManager, LoadsRunsAndUnloadsAPlugin) {
    PluginRoot root;
    root.add("Alpha");
    Engine::ScriptRegistry scripts;
    Engine::Scene scene;
    {
        Engine::PluginManager manager{scripts};
        manager.loadAll({root.path()});
        const auto* status = find(manager, "Alpha");
        ASSERT_NE(status, nullptr);
        ASSERT_EQ(status->state, Engine::PluginState::Loaded) << status->error;
        EXPECT_EQ(status->scriptCount, 1U);
        EXPECT_EQ(status->systemCount, 1U);
        const auto* spinner = scripts.descriptor("AlphaSpinner");
        ASSERT_NE(spinner, nullptr);
        ASSERT_EQ(spinner->fields.size(), 1U);
        EXPECT_EQ(spinner->fields.front().name, "Speed");

        // Systems run only between playStart and playStop.
        const std::size_t updatesBefore = logged("Alpha onUpdate");
        manager.update(scene, 0.016F);
        EXPECT_EQ(logged("Alpha onUpdate"), updatesBefore);
        manager.playStart(scene);
        manager.update(scene, 0.016F);
        manager.fixedUpdate(scene, 0.016F);
        EXPECT_EQ(logged("Alpha onUpdate"), updatesBefore + 1);
        EXPECT_GE(logged("Alpha onPlayStart"), 1U);
        EXPECT_GE(logged("Alpha onFixedUpdate"), 1U);

        // Unloading while playing stops the systems first.
        const std::size_t stopsBefore = logged("Alpha onPlayStop");
        manager.unloadAll(scene);
        EXPECT_EQ(logged("Alpha onPlayStop"), stopsBefore + 1);
        EXPECT_EQ(scripts.descriptor("AlphaSpinner"), nullptr);
        EXPECT_EQ(manager.loadedCount(), 0U);
    }
    EXPECT_GE(logged("[TestPlugin] shutdown"), 1U);
}

TEST(PluginManager, HonoursDisabledPluginsAndDependencies) {
    PluginRoot root;
    root.add("Base");
    root.add("NeedsBase", "dependencies = Base\n");
    root.add("NeedsMissing", "dependencies = Nowhere\n");
    Engine::ScriptRegistry scripts;
    Engine::PluginManager manager{scripts};
    manager.loadAll({root.path()}, {"Base"});
    ASSERT_NE(find(manager, "Base"), nullptr);
    EXPECT_EQ(find(manager, "Base")->state, Engine::PluginState::Disabled);
    EXPECT_EQ(find(manager, "NeedsBase")->state, Engine::PluginState::Failed);
    EXPECT_NE(find(manager, "NeedsBase")->error.find("Base"), std::string::npos);
    EXPECT_EQ(find(manager, "NeedsMissing")->state, Engine::PluginState::Failed);
    EXPECT_NE(find(manager, "NeedsMissing")->error.find("not installed"), std::string::npos);
    EXPECT_EQ(manager.loadedCount(), 0U);
}

TEST(PluginManager, LoadsDependenciesFirst) {
    PluginRoot root;
    // "A..." sorts before its dependency, so order comes from the graph.
    root.add("AppLayer", "dependencies = Zcore\n");
    root.add("Zcore");
    Engine::ScriptRegistry scripts;
    Engine::PluginManager manager{scripts};
    manager.loadAll({root.path()});
    EXPECT_EQ(find(manager, "AppLayer")->state, Engine::PluginState::Loaded) << find(manager, "AppLayer")->error;
    EXPECT_EQ(find(manager, "Zcore")->state, Engine::PluginState::Loaded);
    EXPECT_EQ(manager.loadedCount(), 2U);
    Engine::Registry scene;
    manager.unloadAll(scene);
}

TEST(PluginManager, ReportsFailuresWithoutLoading) {
    PluginRoot root;
    root.add("NoLibrary", {}, false);
    const auto future = root.add("FutureApi");
    std::ofstream{future / "FutureApi.geplugin"} << "name = FutureApi\napi_version = 999\n";
    root.add("EditorOnly", "type = editor\n");
    const auto refusing = root.add("Refusing");
    std::ofstream{refusing / "throw_on_startup"} << "";
    Engine::ScriptRegistry scripts;
    Engine::PluginManager manager{scripts};
    manager.loadAll({root.path()});
    EXPECT_EQ(find(manager, "NoLibrary")->state, Engine::PluginState::Failed);
    EXPECT_NE(find(manager, "NoLibrary")->error.find("No library"), std::string::npos);
    EXPECT_EQ(find(manager, "FutureApi")->state, Engine::PluginState::Failed);
    EXPECT_NE(find(manager, "FutureApi")->error.find("API 999"), std::string::npos);
    EXPECT_EQ(find(manager, "EditorOnly")->state, Engine::PluginState::Failed);
    EXPECT_NE(find(manager, "EditorOnly")->error.find("not supported"), std::string::npos);
    EXPECT_EQ(find(manager, "Refusing")->state, Engine::PluginState::Failed);
    EXPECT_NE(find(manager, "Refusing")->error.find("startup refused by test"), std::string::npos);
    EXPECT_EQ(scripts.descriptor("RefusingSpinner"), nullptr);
    EXPECT_EQ(manager.loadedCount(), 0U);
}

TEST(PluginManager, RefusesScriptNamesThatAreAlreadyRegistered) {
    PluginRoot root;
    root.add("Clash");
    Engine::ScriptRegistry scripts;
    scripts.registerClass(Engine::ScriptClassDescriptor{.name = "ClashSpinner",
                                                        .create = []() -> Engine::Script* { return nullptr; },
                                                        .destroy = [](Engine::Script*) {},
                                                        .sourceFile = {},
                                                        .fields = {},
                                                        .moduleGeneration = 0});
    Engine::PluginManager manager{scripts};
    manager.loadAll({root.path()});
    EXPECT_EQ(find(manager, "Clash")->state, Engine::PluginState::Failed);
    EXPECT_NE(find(manager, "Clash")->error.find("already provided"), std::string::npos);
    // The game's class is untouched.
    ASSERT_NE(scripts.descriptor("ClashSpinner"), nullptr);
    EXPECT_EQ(scripts.descriptor("ClashSpinner")->moduleGeneration, 0U);
}

// A plugin script running in a scene must be destroyed before its library
// is unloaded, keeping its field values for when the plugin returns.
TEST(PluginManager, UnloadReleasesPluginScriptInstancesFirst) {
    PluginRoot root;
    root.add("Live");
    Engine::ScriptRegistry scripts;
    Engine::ScriptSystem system{scripts};
    Engine::Registry scene;
    const auto entity = scene.create();
    scene.add<Engine::Transform>(entity);
    scene.add<Engine::ScriptComponent>(entity, Engine::ScriptComponent{"LiveSpinner"});

    Engine::PluginManager manager{scripts};
    manager.loadAll({root.path()});
    ASSERT_EQ(find(manager, "Live")->state, Engine::PluginState::Loaded) << find(manager, "Live")->error;
    const std::size_t createdBefore = logged("spinner created");
    const std::size_t destroyedBefore = logged("spinner destroyed");
    system.update(scene, 0.016F);
    ASSERT_EQ(logged("spinner created"), createdBefore + 1);

    manager.unloadAll(scene);
    EXPECT_EQ(logged("spinner destroyed"), destroyedBefore + 1);
    const auto& component = scene.get<Engine::ScriptComponent>(entity);
    ASSERT_TRUE(component.fields.contains("Speed"));
    EXPECT_FLOAT_EQ(std::get<float>(component.fields.at("Speed")), 90.0F);
    EXPECT_EQ(scripts.descriptor("LiveSpinner"), nullptr);
}
