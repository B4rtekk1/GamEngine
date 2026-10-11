#include <gtest/gtest.h>
#include <SDL3/SDL.h>
#include "Engine/Physics/GlassDestructionSystem.h"
#include "Engine/Physics/PhysicsSystem.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Scene/SceneEditor.h"
#include <cstdlib>
#include <algorithm>

namespace {
    // GPU integration test: opt in with GE_RUN_RENDERER_TESTS=1 on a Vulkan display.
    TEST(GlassRendering, GameplayFractureUploadsPaneAndDebrisWithoutManualSynchronization) {
        if (!std::getenv("GE_RUN_RENDERER_TESTS"))
            GTEST_SKIP() << "Requires a Vulkan display and compiled renderer assets";
        ASSERT_TRUE(SDL_InitSubSystem(SDL_INIT_VIDEO)) << SDL_GetError();
        struct Window final {
            SDL_Window *handle = SDL_CreateWindow("Glass renderer regression", 320, 180,
                SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
            ~Window() { if (handle) SDL_DestroyWindow(handle); SDL_QuitSubSystem(SDL_INIT_VIDEO); }
        } window;
        ASSERT_NE(window.handle, nullptr) << SDL_GetError();
        Engine::Scene scene;
        Engine::SceneEditor editor(scene);
        const auto camera = scene.createCamera("Camera");
        camera.setPosition({0.0F, 0.0F, 2.0F});
        Engine::GlassDestructionComponent settings;
        settings.maxDynamicFragments = 4;
        const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Glass", settings);
        const auto entity = scene.findEntity(pane.id());
        Engine::Renderer renderer(Engine::RenderConfig{
            .shadowQuality = Engine::ShadowQuality::Low,
            .gtaoQuality = Engine::GtaoQuality::Low,
            .iblQuality = Engine::IblQuality::Low});
        const auto projectRoot = std::filesystem::temp_directory_path() / "gamengine-glass-rendering-test";
        std::filesystem::create_directories(projectRoot / "Assets/BRDF");
        std::filesystem::copy_file(std::filesystem::path{SDL_GetBasePath()} / "Assets/BRDF/brdf_lut.bin",
            projectRoot / "Assets/BRDF/brdf_lut.bin", std::filesystem::copy_options::overwrite_existing);
        renderer.setProjectRoot(projectRoot);
        renderer.initialize(scene, window.handle);
        const auto originalHandle = editor.read<Engine::MeshRendererComponent>(entity).mesh;
        ASSERT_TRUE(originalHandle.uploaded());
        renderer.renderFrame();
        ASSERT_TRUE(scene.physics().shootRay({0.0F, 0.0F, 2.0F}, {0.0F, 0.0F, -1.0F}));
        const auto fracturedHandle = editor.read<Engine::MeshRendererComponent>(entity).mesh;
        ASSERT_NE(fracturedHandle.resource().get(), originalHandle.resource().get());
        EXPECT_FALSE(fracturedHandle.uploaded());
        std::size_t debrisCount = 0;
        editor.view<Engine::GlassDebrisComponent, Engine::MeshRendererComponent>(
            [&](auto, const auto &, const auto &mesh) { ++debrisCount; EXPECT_FALSE(mesh.mesh.uploaded()); });
        ASSERT_GT(debrisCount, 0);
        renderer.renderFrame();
        EXPECT_TRUE(fracturedHandle.uploaded());
        EXPECT_EQ(fracturedHandle.resource()->indexCount, fracturedHandle.source()->indexCount());
        editor.view<Engine::GlassDebrisComponent, Engine::MeshRendererComponent>(
            [&](auto, const auto &, const auto &mesh) { EXPECT_TRUE(mesh.mesh.uploaded()); });
        // A later hit changes visibility without replacing/reuploading the heap range.
        const auto firstVertex = fracturedHandle.resource()->firstVertex;
        const auto firstIndex = fracturedHandle.resource()->firstIndex;
        const auto glass = editor.read<Engine::GlassDestructionComponent>(entity);
        const auto next = std::ranges::find_if(glass.fragments, [](const auto &fragment) { return fragment.attached; });
        ASSERT_NE(next, glass.fragments.end());
        ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, entity,
            {.worldPosition = {next->center.x, next->center.y, 0.0F}}));
        renderer.renderFrame();
        EXPECT_EQ(editor.read<Engine::MeshRendererComponent>(entity).mesh.resource(), fracturedHandle.resource());
        EXPECT_EQ(fracturedHandle.resource()->firstVertex, firstVertex);
        EXPECT_EQ(fracturedHandle.resource()->firstIndex, firstIndex);
        Engine::PhysicsSystem physics;
        physics.update(scene, 1.0F / 60.0F);
        renderer.renderFrame();
        Engine::GlassDestructionSystem::update(scene, 5.0F);
        renderer.renderFrame();
        renderer.shutdown();
    }
}
