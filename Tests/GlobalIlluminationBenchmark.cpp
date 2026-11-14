#include "Engine/Core/Profiler.h"
#include "Engine/Renderer/Geometry/Meshlet.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scene/Scene.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {
// GPU benchmark: opt in with GE_RUN_RENDERER_TESTS=1 on a Vulkan display.
// Renders one room-like scene with SSGI and then with DDGI and reports the
// median GPU time of each pass at the same resolution.
TEST(GlobalIllumination, SsgiAndDdgiGpuTime) {
    if (!std::getenv("GE_RUN_RENDERER_TESTS"))
        GTEST_SKIP() << "Requires a Vulkan display and compiled renderer assets";
    ASSERT_TRUE(SDL_InitSubSystem(SDL_INIT_VIDEO)) << SDL_GetError();
    constexpr int Width = 1920, Height = 1080;
    struct Window final {
        SDL_Window* handle = SDL_CreateWindow("GI benchmark", Width, Height, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
        ~Window() {
            if (handle)
                SDL_DestroyWindow(handle);
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    } window;
    ASSERT_NE(window.handle, nullptr) << SDL_GetError();

    Engine::Scene scene;
    const auto camera = scene.createCamera("Camera");
    camera.setPosition({0.0F, 2.0F, 9.0F});
    camera.setRotation({-8, -90, 0});
    // Procedural cubes carry no meshlets; DDGI traces meshlet clusters. Each
    // actor gets its own mesh: createMesh does not share one source mesh.
    const auto box = [&](const std::string& name, Engine::Vec3 position, Engine::Vec3 scale, Engine::Math::Color color) {
        auto mesh = std::make_shared<Engine::Mesh>(*Engine::Prefab::cube().mesh());
        ASSERT_TRUE(Engine::build_meshlets(*mesh));
        const auto actor = scene.createMesh(name, std::move(mesh),
                                            Engine::PBRMaterial{.baseColor = color, .roughness = 0.8F});
        actor.setPosition(position);
        actor.setScale(scale);
    };
    box("Floor", {0, -0.5F, 0}, {20, 1, 20}, {0.8F, 0.8F, 0.8F});
    box("Red wall", {-6, 3, 0}, {1, 7, 20}, {0.8F, 0.1F, 0.1F});
    box("Green wall", {6, 3, 0}, {1, 7, 20}, {0.1F, 0.8F, 0.1F});
    box("Back wall", {0, 3, -6}, {12, 7, 1}, {0.8F, 0.8F, 0.8F});
    for (int i = 0; i < 64; ++i)
        box("Crate " + std::to_string(i), {float(i % 8) * 1.4F - 5.0F, 0.5F, float(i / 8) * 1.4F - 5.0F},
            {0.7F, 1.0F + float(i % 3) * 0.5F, 0.7F}, {0.9F, 0.7F, 0.3F});
    // Height-displaced slabs exercise cluster templates: their CLASes are
    // instantiated from the cube's templates with displaced positions.
    {
        auto mesh = std::make_shared<Engine::Mesh>(*Engine::Prefab::cube().mesh());
        Engine::Mesh::Image height{.width = 16, .height = 16};
        for (std::uint32_t y = 0; y < height.height; ++y)
            for (std::uint32_t x = 0; x < height.width; ++x) {
                const auto value = static_cast<std::uint8_t>((x * 16 + y * 7) & 0xff);
                height.rgbaPixels.insert(height.rgbaPixels.end(), {value, value, value, 255});
            }
        mesh->images.push_back(std::move(height));
        ASSERT_TRUE(Engine::build_meshlets(*mesh));
        const std::shared_ptr<const Engine::Mesh> displacedCube = mesh;
        const auto slab = scene.createMesh("Displaced slab", displacedCube,
                                           Engine::PBRMaterial{.baseColor = {0.3F, 0.3F, 0.9F},
                                                               .roughness = 0.6F,
                                                               .displacementTexture = 0,
                                                               .displacementScale = 0.2F});
        slab.setPosition({0.0F, 3.5F, -4.5F});
        slab.setScale({2.0F, 0.4F, 1.0F});
    }
    Engine::LightComponent sun;
    sun.type = Engine::LightType::Directional;
    sun.intensity = 3.0F;
    const auto light = scene.createLight("Sun", sun);
    light.setRotation({-50, 30, 0});

    // Sequential keeps GTAO/SSGI on the graphics queue, where timestamps are collected.
    Engine::Renderer renderer(Engine::RenderConfig{.renderPath = Engine::RenderPath::Sequential,
                                                   .antialiasing = Engine::AntialiasingLevel::TAA,
                                                   .shadowQuality = Engine::ShadowQuality::Medium,
                                                   .gtaoQuality = Engine::GtaoQuality::Medium,
                                                   .iblQuality = Engine::IblQuality::Low,
                                                   .ssgi = {.quality = Engine::SsgiQuality::Medium}});
    const auto projectRoot = std::filesystem::temp_directory_path() / "gamengine-gi-benchmark";
    std::filesystem::create_directories(projectRoot / "Assets/BRDF");
    std::filesystem::copy_file(std::filesystem::path{SDL_GetBasePath()} / "Assets/BRDF/brdf_lut.bin",
                               projectRoot / "Assets/BRDF/brdf_lut.bin",
                               std::filesystem::copy_options::overwrite_existing);
    renderer.setProjectRoot(projectRoot);
    renderer.initialize(scene, window.handle);

    const auto measure = [&](const char* label, std::uint32_t frames) {
        std::map<std::string, std::vector<float>> zones;
        std::vector<float> totals;
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            renderer.renderFrame();
            const auto profile = renderer.gpuProfile();
            if (!profile || frame < 30)
                continue;
            totals.push_back(profile->frameMilliseconds);
            for (const auto& event : profile->events) {
                const std::string name{Engine::Profiler::name(event.name)};
                if (name == "SSGI" || name == "SSGI.Capture" || name == "DDGI" || name == "GTAO" || name == "Forward")
                    zones[name].push_back(event.endMs - event.startMs);
            }
            if (renderer.ddgiSettings().enabled)
                zones["traced probes"].push_back(static_cast<float>(renderer.ddgiStatistics().tracedProbes));
        }
        const auto median = [](std::vector<float> values) {
            if (values.empty())
                return 0.0F;
            std::ranges::sort(values);
            return values[values.size() / 2];
        };
        std::cout << "[GI benchmark] " << label << ": GPU frame " << median(totals) << " ms";
        for (const auto& [name, values] : zones)
            std::cout << ", " << name << " " << median(values) << (name == "traced probes" ? "" : " ms");
        std::cout << '\n';
        return zones;
    };

    const auto ssgi = measure("SSGI Medium", 150);
    EXPECT_FALSE(ssgi.contains("DDGI"));
    EXPECT_TRUE(ssgi.contains("SSGI"));
    if (!renderer.ddgiSupported()) {
        renderer.shutdown();
        GTEST_SKIP() << "DDGI needs cluster ray tracing (VK_NV_cluster/partitioned_acceleration_structure)";
    }
    for (const auto quality : {Engine::DdgiQuality::Low, Engine::DdgiQuality::Medium, Engine::DdgiQuality::High}) {
        renderer.setDdgiSettings({.enabled = true, .quality = quality});
        const char* label = quality == Engine::DdgiQuality::Low      ? "DDGI Low (96 rays, 1/4 probes per frame)"
                            : quality == Engine::DdgiQuality::Medium ? "DDGI Medium (128 rays, 1/2 probes per frame)"
                                                                     : "DDGI High (192 rays, every probe)";
        const auto ddgi = measure(label, 150);
        EXPECT_TRUE(ddgi.contains("DDGI"));
        EXPECT_FALSE(ddgi.contains("SSGI"));
    }
    // Probe cache: with nothing changing, converged probes go to sleep.
    renderer.setDdgiSettings({.enabled = true, .quality = Engine::DdgiQuality::Medium});
    measure("DDGI Medium, converging", 300);
    const auto cached = measure("DDGI Medium, cached (static scene)", 150);
    // Without the cache, Medium traces half of the 2048 probes every frame.
    auto tracedCached = cached.at("traced probes");
    std::ranges::sort(tracedCached);
    EXPECT_LT(tracedCached[tracedCached.size() / 2], 1024.0F);
    // A moving crate wakes only the probes around it.
    auto crate = scene.findActor("Crate 27");
    ASSERT_TRUE(crate.valid());
    std::uint32_t step = 0;
    const auto moving = [&](const char* label, std::uint32_t frames) {
        std::map<std::string, std::vector<float>> zones;
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            crate.setPosition({-0.8F + 0.5F * std::sin(float(step++) * 0.05F), 0.5F, -0.8F});
            renderer.renderFrame();
            const auto profile = renderer.gpuProfile();
            if (!profile || frame < 30)
                continue;
            for (const auto& event : profile->events)
                if (Engine::Profiler::name(event.name) == "DDGI")
                    zones["DDGI"].push_back(event.endMs - event.startMs);
            zones["traced probes"].push_back(static_cast<float>(renderer.ddgiStatistics().tracedProbes));
        }
        for (auto& [name, values] : zones)
            std::ranges::sort(values);
        std::cout << "[GI benchmark] " << label << ": DDGI " << zones["DDGI"][zones["DDGI"].size() / 2]
                  << " ms, traced probes " << zones["traced probes"][zones["traced probes"].size() / 2] << '\n';
    };
    moving("DDGI Medium, cached, one crate moving", 150);
    const auto stats = renderer.ddgiStatistics();
    std::cout << "[GI benchmark] Mega Geometry scene: " << stats.clusters << " clusters, " << stats.clusterTemplates
              << " templates, " << stats.displacedClusters << " displaced clusters, " << stats.clusterBlases
              << " cluster BLASes; memory " << stats.sceneBytesBuilt / 1024 << " KiB built -> "
              << stats.sceneBytesCompacted / 1024 << " KiB compacted\n";
    EXPECT_TRUE(stats.compacted);
    EXPECT_GT(stats.clusterTemplates, 0U);
    EXPECT_GT(stats.displacedClusters, 0U);
    EXPECT_LT(stats.sceneBytesCompacted, stats.sceneBytesBuilt);
    renderer.shutdown();
}
} // namespace
