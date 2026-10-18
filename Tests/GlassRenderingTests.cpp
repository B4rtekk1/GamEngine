#include "Engine/Physics/GlassDestructionSystem.h"
#include "Engine/Physics/PhysicsSystem.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Scene/SceneEditor.h"
#include "Engine/Scene/SceneSerializer.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <sstream>

namespace {
// GPU integration test: opt in with GE_RUN_RENDERER_TESTS=1 on a Vulkan display.
TEST(GlassRendering, GameplayFractureUploadsPaneAndDebrisWithoutManualSynchronization) {
    if (!std::getenv("GE_RUN_RENDERER_TESTS"))
        GTEST_SKIP() << "Requires a Vulkan display and compiled renderer assets";
    ASSERT_TRUE(SDL_InitSubSystem(SDL_INIT_VIDEO)) << SDL_GetError();
    struct Window final {
        SDL_Window* handle =
            SDL_CreateWindow("Glass renderer regression", 320, 180, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
        ~Window() {
            if (handle)
                SDL_DestroyWindow(handle);
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    } window;
    ASSERT_NE(window.handle, nullptr) << SDL_GetError();
    Engine::Scene scene;
    Engine::SceneEditor editor(scene);
    const auto camera = scene.createCamera("Camera");
    camera.setPosition({0.0F, 0.0F, 2.0F});
    camera.setRotation({0, -90, 0});
    Engine::GlassDestructionComponent settings;
    settings.maxDynamicFragments = 4;
    const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Glass", settings);
    const auto entity = scene.findEntity(pane.id());
    Engine::Renderer renderer(Engine::RenderConfig{.shadowQuality = Engine::ShadowQuality::Low,
                                                   .gtaoQuality = Engine::GtaoQuality::Low,
                                                   .iblQuality = Engine::IblQuality::Low});
    const auto projectRoot = std::filesystem::temp_directory_path() / "gamengine-glass-rendering-test";
    std::filesystem::create_directories(projectRoot / "Assets/BRDF");
    std::filesystem::copy_file(std::filesystem::path{SDL_GetBasePath()} / "Assets/BRDF/brdf_lut.bin",
                               projectRoot / "Assets/BRDF/brdf_lut.bin",
                               std::filesystem::copy_options::overwrite_existing);
    renderer.setProjectRoot(projectRoot);
    renderer.initialize(scene, window.handle);
    const auto originalHandle = editor.read<Engine::MeshRendererComponent>(entity).mesh;
    ASSERT_TRUE(originalHandle.uploaded());
    renderer.renderFrame();
    auto topology = editor.renderTopologyRevision();
    ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, entity, {.worldPosition = {-0.5F, 0, 0}, .energy = 2}));
    renderer.renderFrame();
    EXPECT_NE(editor.read<Engine::MeshRendererComponent>(entity).mesh.resource(), originalHandle.resource());
    EXPECT_GT(editor.renderTopologyRevision(), topology);
    const auto graphHandle = editor.read<Engine::MeshRendererComponent>(entity).mesh;
    EXPECT_TRUE(graphHandle.uploaded());
    topology = editor.renderTopologyRevision();
    ASSERT_EQ(editor.read<Engine::GlassDestructionComponent>(entity).cracks.size(), 1);
    const auto impactStart = std::chrono::steady_clock::now();
    ASSERT_TRUE(scene.physics().shootRay({0.0F, 0.0F, 2.0F}, {0.0F, 0.0F, -1.0F}));
    const auto fracturedHandle = editor.read<Engine::MeshRendererComponent>(entity).mesh;
    ASSERT_NE(fracturedHandle.resource(), graphHandle.resource());
    EXPECT_GT(editor.renderTopologyRevision(), topology);
    topology = editor.renderTopologyRevision();
    std::size_t debrisCount = 0;
    editor.view<Engine::GlassDebrisComponent, Engine::GlassFragmentBindingComponent>(
        [&](auto shard, const auto&, const auto& binding) {
            ++debrisCount;
            EXPECT_FALSE(editor.has<Engine::MeshRendererComponent>(shard));
            EXPECT_EQ(binding.paneEntity, entity);
        });
    ASSERT_GT(debrisCount, 0);
    renderer.renderFrame();
    std::cout << "Glass impact + frame: "
              << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - impactStart).count()
              << " ms\n";
    EXPECT_TRUE(fracturedHandle.uploaded());
    EXPECT_EQ(fracturedHandle.resource()->indexCount, fracturedHandle.source()->indexCount());
    // A later hit refines surviving glass and uploads new packed sections.
    const auto hitTopology = editor.renderTopologyRevision();
    const auto glass = editor.read<Engine::GlassDestructionComponent>(entity);
    const auto next = std::ranges::find_if(glass.fragments, [](const auto& fragment) { return fragment.attached; });
    ASSERT_NE(next, glass.fragments.end());
    ASSERT_TRUE(Engine::GlassDestructionSystem::onImpact(scene, entity,
                                                         {.worldPosition = {next->center.x, next->center.y, 0.0F}}));
    renderer.renderFrame();
    EXPECT_GT(editor.renderTopologyRevision(), hitTopology);
    const auto refinedHandle = editor.read<Engine::MeshRendererComponent>(entity).mesh;
    EXPECT_NE(refinedHandle.resource(), fracturedHandle.resource());
    EXPECT_TRUE(refinedHandle.uploaded());
    EXPECT_EQ(refinedHandle.resource()->metadata.sections.size(),
              editor.read<Engine::GlassDestructionComponent>(entity).fragments.size() + 1);
    const auto refinedTopology = editor.renderTopologyRevision();
    Engine::PhysicsSystem physics;
    physics.update(scene, 1.0F / 60.0F);
    renderer.renderFrame();
    Engine::GlassDestructionSystem::update(scene, 5.0F);
    renderer.renderFrame();
    EXPECT_EQ(editor.renderTopologyRevision(), refinedTopology);
    renderer.shutdown();
}
TEST(GlassRendering, LoadedPaneReplacementRetiresBatchesBeforeDrawing) {
    if (!std::getenv("GE_RUN_RENDERER_TESTS"))
        GTEST_SKIP() << "Requires a Vulkan display";
    ASSERT_TRUE(SDL_InitSubSystem(SDL_INIT_VIDEO)) << SDL_GetError();
    struct Window final {
        SDL_Window* handle =
            SDL_CreateWindow("Loaded glass regression", 320, 180, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
        ~Window() {
            if (handle)
                SDL_DestroyWindow(handle);
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    } window;
    ASSERT_NE(window.handle, nullptr) << SDL_GetError();
    Engine::Scene authored;
    const auto camera = authored.createCamera("Camera");
    camera.setPosition({0, 0, 2});
    camera.setRotation({0, -90, 0});
    Engine::GlassDestructionComponent settings;
    settings.maxDynamicFragments = 0;
    (void)Engine::GlassDestructionSystem::createPane(authored, "Loaded glass", settings);
    std::ostringstream output;
    Engine::SceneSerializer::save(authored, output);
    Engine::Scene scene;
    std::istringstream input(output.str());
    Engine::SceneSerializer::load(scene, input);
    Engine::SceneEditor editor(scene);
    const auto entity = scene.findEntity(scene.findActor("Loaded glass").id());
    Engine::Renderer renderer(Engine::RenderConfig{.shadowQuality = Engine::ShadowQuality::Low,
                                                   .gtaoQuality = Engine::GtaoQuality::Low,
                                                   .iblQuality = Engine::IblQuality::Low});
    const auto root = std::filesystem::temp_directory_path() / "gamengine-loaded-glass-test";
    std::filesystem::create_directories(root / "Assets/BRDF");
    std::filesystem::copy_file(std::filesystem::path{SDL_GetBasePath()} / "Assets/BRDF/brdf_lut.bin",
                               root / "Assets/BRDF/brdf_lut.bin", std::filesystem::copy_options::overwrite_existing);
    renderer.setProjectRoot(root);
    renderer.initialize(scene, window.handle);
    renderer.renderFrame();
    const std::weak_ptr oldResource = editor.read<Engine::MeshRendererComponent>(entity).mesh.resource();
    ASSERT_TRUE(
        Engine::GlassDestructionSystem::onImpact(scene, entity, {.worldPosition = {0.45F, 0.2F, 0}, .energy = 40}));
    ASSERT_TRUE(oldResource.expired()) << "Reproduction requires the old CPU mesh identity to be released";
    renderer.renderFrame();
    renderer.renderFrame();
    EXPECT_TRUE(editor.read<Engine::MeshRendererComponent>(entity).mesh.uploaded());
    const auto checkAtlasLayers = [&](unsigned layers) {
        unsigned atlases = 0;
        constexpr std::uint64_t bytesPerLayer = 256ULL * 256 * 4;
        for (const auto& allocation : renderer.gpuSceneMemory()) {
            if (allocation.table == "Glass crack atlas") {
                ++atlases;
                EXPECT_GE(allocation.bytes, layers * bytesPerLayer);
                EXPECT_LT(allocation.bytes, (layers + 1) * bytesPerLayer);
            } else if (allocation.table == "Glass crack metadata") {
                EXPECT_LT(allocation.bytes, (layers + 1) * 129ULL * sizeof(glm::vec4) + 4096);
            }
        }
        EXPECT_GT(atlases, 0U);
    };
    checkAtlasLayers(1);
    const auto second = Engine::GlassDestructionSystem::createPane(scene, "Second", settings);
    second.setPosition({3, 0, 0});
    const auto third = Engine::GlassDestructionSystem::createPane(scene, "Third", settings);
    third.setPosition({-3, 0, 0});
    renderer.renderFrame();
    renderer.renderFrame();
    checkAtlasLayers(3);
    const auto thirdEntity = scene.findEntity(third.id());
    EXPECT_EQ(editor.read<Engine::GlassDestructionComponent>(thirdEntity).crackAtlasIndex, 2U);
    scene.destroy(scene.findEntity(second.id()));
    renderer.renderFrame();
    renderer.renderFrame();
    checkAtlasLayers(2);
    EXPECT_EQ(editor.read<Engine::GlassDestructionComponent>(thirdEntity).crackAtlasIndex, 1U);
    ASSERT_TRUE(
        Engine::GlassDestructionSystem::onImpact(scene, thirdEntity, {.worldPosition = {-2.6F, 0.2F, 0}, .energy = 2}));
    renderer.renderFrame();
    scene.destroy(thirdEntity);
    renderer.renderFrame();
    checkAtlasLayers(1);
    scene.destroy(entity);
    renderer.renderFrame();
    checkAtlasLayers(1); // Vulkan descriptors retain one empty layer.
    renderer.shutdown();
}

TEST(GlassRendering, ScalingBenchmark) {
    if (!std::getenv("GE_RUN_GLASS_BENCHMARKS"))
        GTEST_SKIP() << "Set GE_RUN_GLASS_BENCHMARKS=1 on a Vulkan display";
    ASSERT_TRUE(SDL_InitSubSystem(SDL_INIT_VIDEO)) << SDL_GetError();
    struct Window {
        SDL_Window* handle =
            SDL_CreateWindow("Glass scaling benchmark", 640, 360, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
        ~Window() {
            if (handle)
                SDL_DestroyWindow(handle);
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    } window;
    ASSERT_NE(window.handle, nullptr);
    const auto root = std::filesystem::temp_directory_path() / "gamengine-glass-rendering-test";
    std::filesystem::create_directories(root / "Assets/BRDF");
    std::filesystem::copy_file(std::filesystem::path{SDL_GetBasePath()} / "Assets/BRDF/brdf_lut.bin",
                               root / "Assets/BRDF/brdf_lut.bin", std::filesystem::copy_options::overwrite_existing);
    const char* path = std::getenv("GE_GLASS_BENCHMARK_OUTPUT");
    std::ofstream output(path ? path : "/tmp/glass-gpu-benchmark.csv");
    ASSERT_TRUE(output.good());
    output << "panes,state,p99_frame_wall_ms,peak_frame_wall_ms,scene_sync_wall_ms,mean_glass_gpu_ms,mean_culling_gpu_"
              "ms,mean_crack_atlas_gpu_ms,gpu_scene_bytes,new_convex_cooks,peak_debris,fracture_generation_cpu_ms,bond_"
              "solver_cpu_ms,debris_spawn_cpu_ms\n";
    for (unsigned count : {1U, 10U, 100U, 500U})
        for (unsigned state = 0; state < 3; ++state) {
            if (const char* only = std::getenv("GE_GLASS_BENCHMARK_PANES");
                only && std::strtoul(only, nullptr, 10) != count)
                continue;
            Engine::Scene scene;
            Engine::SceneEditor editor(scene);
            const auto camera = scene.createCamera("Camera");
            camera.setRotation({0, -90, 0});
            std::vector<Engine::Entity> panes;
            Engine::GlassDestructionComponent settings;
            settings.maxDynamicFragments = 8;
            for (unsigned i = 0; i < count; ++i) {
                const auto pane =
                    Engine::GlassDestructionSystem::createPane(scene, "Pane " + std::to_string(i), settings);
                pane.setPosition({float(i % 25) * 2.2F, float(i / 25) * 2.2F, 0});
                panes.push_back(scene.findEntity(pane.id()));
            }
            const float columns = float(std::min(25U, count)), rows = float((count + 24) / 25);
            const float distance = std::max(columns * 2.2F / 1.8F, rows * 2.2F) * 1.1F;
            camera.setPosition({(columns - 1) * 1.1F, (rows - 1) * 1.1F, std::max(4.0F, distance)});
            Engine::PhysicsSystem physics;
            physics.update(scene, 1.0F / 60);
            Engine::Renderer renderer(Engine::RenderConfig{.shadowQuality = Engine::ShadowQuality::Low,
                                                           .gtaoQuality = Engine::GtaoQuality::Low,
                                                           .iblQuality = Engine::IblQuality::Low});
            renderer.setProjectRoot(root);
            renderer.initialize(scene, window.handle);
            // Exclude initial pipeline/cache warmup from the impact frame distribution.
            for (unsigned frame = 0; frame < 4; ++frame)
                renderer.renderFrame();
            const auto before = physics.diagnostics();
            const auto stableTopology = editor.renderTopologyRevision();
            std::vector<double> frames;
            double syncMs = 0, glassMs = 0, cullingMs = 0, crackMs = 0;
            std::size_t gpuSamples = 0, peakDebris = 0;
            std::uint64_t lastGpuFrame = ~std::uint64_t{0};
            for (unsigned frame = 0; frame < 120; ++frame) {
                const auto start = std::chrono::steady_clock::now();
                Engine::Profiler::beginFrame();
                if (frame == 0 && state != 0) {
                    for (unsigned i = 0; i < (state == 1 ? 1U : count); ++i) {
                        const auto position = editor.read<Engine::Transform>(panes[i]).position;
                        (void)Engine::GlassDestructionSystem::onImpact(scene, panes[i], {.worldPosition = position});
                    }
                    EXPECT_EQ(editor.renderTopologyRevision(), stableTopology);
                    const auto sync = std::chrono::steady_clock::now();
                    renderer.synchronizeScene(scene);
                    syncMs +=
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sync).count();
                }
                physics.update(scene, 1.0F / 60);
                renderer.renderFrame();
                Engine::Profiler::endFrame();
                if (frame == 0 && state == 2 && Engine::Profiler::historySize()) {
                    const auto& profile = Engine::Profiler::historyFrame(Engine::Profiler::historySize() - 1);
                    for (const auto& event : profile.cpuEvents) {
                        const auto name = Engine::Profiler::name(event.name);
                        if (name == "PhysicsActorSyncCPU" || name == "PhysicsStatePushCPU" ||
                            name == "RenderableUpdateCPU" || name == "GPUSceneUploadCPU" ||
                            name == "PhysXSimulationCPU" || name == "RenderableRecordsCPU" ||
                            name == "RenderableBoundsCPU" || name == "RenderableUploadCPU")
                            std::cout << count << " panes: " << name << " " << double(event.endNs - event.startNs) / 1e6
                                      << " ms\n";
                    }
                }
                frames.push_back(
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
                if (const auto gpu = renderer.gpuProfile(); gpu && gpu->frameNumber != lastGpuFrame) {
                    lastGpuFrame = gpu->frameNumber;
                    ++gpuSamples;
                    for (const auto& event : gpu->events) {
                        const auto name = Engine::Profiler::name(event.name);
                        const auto duration = event.endMs - event.startMs;
                        if (name == "GlassThin")
                            glassMs += duration;
                        else if (name == "Culling")
                            cullingMs += duration;
                        else if (name == "GlassCrackAtlasGPU")
                            crackMs += duration;
                    }
                }
                std::size_t debris = 0;
                editor.view<Engine::GlassDebrisComponent>([&](auto, const auto&) { ++debris; });
                peakDebris = std::max(peakDebris, debris);
            }
            std::ranges::sort(frames);
            std::uint64_t bytes = 0;
            for (const auto& allocation : renderer.gpuSceneMemory())
                bytes += allocation.bytes;
            double generation = 0, solver = 0, spawn = 0;
            editor.view<Engine::GlassDestructionComponent>([&](auto, const auto& glass) {
                generation += glass.fractureGenerationCpuMs;
                solver += glass.bondSolverCpuMs;
                spawn += glass.debrisSpawnCpuMs;
            });
            const double samples = double(std::max<std::size_t>(1, gpuSamples));
            output << count << ','
                   << (state == 0   ? "intact"
                       : state == 1 ? "single_hit"
                                    : "simultaneous_hits")
                   << ',' << frames[118] << ',' << frames.back() << ',' << syncMs << ',' << glassMs / samples << ','
                   << cullingMs / samples << ',' << crackMs / samples << ',' << bytes << ','
                   << physics.diagnostics().convexCookCount - before.convexCookCount << ',' << peakDebris << ','
                   << generation << ',' << solver << ',' << spawn << '\n';
            output.flush();
            renderer.shutdown();
        }
}

} // namespace
