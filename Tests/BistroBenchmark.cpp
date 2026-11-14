#include "Engine/Core/Profiler.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Assets/Content.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {
float halfToFloat(std::uint16_t h) {
    const std::uint32_t sign = (h >> 15) & 1U, exponent = (h >> 10) & 31U, mantissa = h & 1023U;
    std::uint32_t bits;
    if (exponent == 0)
        bits = mantissa == 0 ? sign << 31 : 0;
    else if (exponent == 31)
        bits = (sign << 31) | 0x7f800000U;
    else
        bits = (sign << 31) | ((exponent + 112) << 23) | (mantissa << 13);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// Stands in for the editor's ImGui backend so the renderer takes its editor
// path (Scene View) without drawing any UI.
class NullEditorUi final : public Engine::EditorUiBackend {
  public:
    bool initialize(void*, const Engine::EditorUiInitInfo&) override {
        return true;
    }
    void shutdown() noexcept override {}
    void processEvent(const void*) override {}
    [[nodiscard]] bool wantsTextInput() const override {
        return false;
    }
    void newFrame() override {}
    [[nodiscard]] std::uintptr_t addTexture(std::uint64_t, std::uint32_t) override {
        return ++textures_;
    }
    void removeTexture(std::uintptr_t) noexcept override {}
    void renderDrawData(std::uint64_t) override {}

  private:
    std::uintptr_t textures_{};
};

struct SmallFrame final {
    static constexpr int Width = 480, Height = 270;
    std::vector<float> luminance = std::vector<float>(Width * Height);
    std::vector<std::uint8_t> rgb = std::vector<std::uint8_t>(Width * Height * 3);
};

SmallFrame downsample(std::uint32_t width, std::uint32_t height, const std::vector<std::uint16_t>& pixels) {
    SmallFrame frame;
    for (int y = 0; y < SmallFrame::Height; ++y)
        for (int x = 0; x < SmallFrame::Width; ++x) {
            const std::size_t sx = std::size_t(x) * width / SmallFrame::Width;
            const std::size_t sy = std::size_t(y) * height / SmallFrame::Height;
            const std::size_t i = (sy * width + sx) * 4;
            std::array<float, 3> c{halfToFloat(pixels[i]), halfToFloat(pixels[i + 1]), halfToFloat(pixels[i + 2])};
            const float l = 0.2126F * c[0] + 0.7152F * c[1] + 0.0722F * c[2];
            frame.luminance[std::size_t(y) * SmallFrame::Width + x] = l;
            for (int k = 0; k < 3; ++k) {
                const float tone = std::pow(std::max(c[k], 0.0F) / (1.0F + std::max(c[k], 0.0F)), 1.0F / 2.2F);
                frame.rgb[(std::size_t(y) * SmallFrame::Width + x) * 3 + k] = std::uint8_t(std::clamp(tone, 0.0F, 1.0F) * 255.0F);
            }
        }
    return frame;
}

void writePpm(const std::filesystem::path& path, const SmallFrame& frame) {
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << SmallFrame::Width << ' ' << SmallFrame::Height << "\n255\n";
    out.write(reinterpret_cast<const char*>(frame.rgb.data()), static_cast<std::streamsize>(frame.rgb.size()));
}

// GPU benchmark on a real scene: GE_BISTRO_SCENE=<path to a .scene> on a
// Vulkan display. The camera flies a fixed loop through the Bistro street so
// every run sees the same views; per-pass GPU medians are printed.
TEST(BistroBenchmark, FlythroughGpuTime) {
    const char* scenePath = std::getenv("GE_BISTRO_SCENE");
    if (!scenePath)
        GTEST_SKIP() << "Set GE_BISTRO_SCENE to a scene file";
    ASSERT_TRUE(SDL_InitSubSystem(SDL_INIT_VIDEO)) << SDL_GetError();
    constexpr int Width = 1920, Height = 1080;
    struct Window final {
        SDL_Window* handle = SDL_CreateWindow("Bistro benchmark", Width, Height, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
        ~Window() {
            if (handle)
                SDL_DestroyWindow(handle);
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    } window;
    ASSERT_NE(window.handle, nullptr) << SDL_GetError();

    const std::filesystem::path path{scenePath};
    Engine::Scene scene;
    scene.load(path);
    // Same defaults as the Player.
    Engine::RenderConfig config{.features = Engine::RenderFeatures{.shadows = true, .occlusionCulling = true},
                                .renderPath = std::getenv("GE_BISTRO_SEQUENTIAL") ? Engine::RenderPath::Sequential
                                                                                  : Engine::RenderPath::AsyncCompute,
                                .antialiasing = std::getenv("GE_BISTRO_NOAA") ? Engine::AntialiasingLevel::Off
                                                                             : Engine::AntialiasingLevel::TAA,
                                .shadowQuality = Engine::ShadowQuality::High,
                                .gtaoQuality = Engine::GtaoQuality::High,
                                .iblQuality = Engine::IblQuality::High};
    // GE_BISTRO_NO_MESH_SHADERS: indexed meshlet draws instead of
    // VK_EXT_mesh_shader (forward, prepass and VSM casters), for A/B timing.
    config.features.meshShaders = std::getenv("GE_BISTRO_NO_MESH_SHADERS") == nullptr;
    Engine::Renderer renderer(config);
    const auto projectRoot = path.parent_path().parent_path().parent_path();
    renderer.setProjectRoot(projectRoot);
    // GE_BISTRO_SCENE_VIEW: render the editor's Scene View instead, flown
    // with the editor camera along the same path.
    const bool sceneView = std::getenv("GE_BISTRO_SCENE_VIEW") != nullptr;
    NullEditorUi editorUi;
    if (sceneView)
        renderer.setEditorUiBackend(&editorUi);
    renderer.initialize(scene, window.handle);
    if (!scene.environmentEquirectangular().empty())
        ASSERT_TRUE(renderer.setEnvironmentEquirectangular(path.parent_path().parent_path() /
                                                           scene.environmentEquirectangular()));
    if (const char* gi = std::getenv("GE_BISTRO_GI")) {
        auto settings = renderer.globalIllumination();
        const std::string mode{gi};
        settings.mode = mode == "ddgi"  ? Engine::GlobalIlluminationMode::RayTracedProbes
                        : mode == "off" ? Engine::GlobalIlluminationMode::Off
                                        : Engine::GlobalIlluminationMode::ScreenSpace;
        renderer.setGlobalIllumination(settings);
    }
    // GE_BISTRO_GTAO=<GtaoQuality index> (4 PerformanceHalf, 5 BalancedHalf),
    // GE_BISTRO_SSGI=<SsgiQuality index> (0 off .. 3 high): price the presets.
    if (const char* gtao = std::getenv("GE_BISTRO_GTAO")) {
        // The GTAO kernels are rebuilt with the attachments, as the editor's
        // reload does after a preset change.
        renderer.setGtaoQuality(static_cast<Engine::GtaoQuality>(std::atoi(gtao)));
        renderer.reconfigureAntialiasing();
    }
    if (const char* ssgi = std::getenv("GE_BISTRO_SSGI")) {
        auto settings = renderer.ssgiSettings();
        settings.quality = static_cast<Engine::SsgiQuality>(std::atoi(ssgi));
        renderer.setSsgiSettings(settings);
    }
    // GE_BISTRO_PBR_DEBUG=<PbrDebugView index>: capture material channels
    // (5 albedo, 6 normals, 7 roughness, 8 metallic, 9 AO) instead of lighting.
    if (const char* debugView = std::getenv("GE_BISTRO_PBR_DEBUG"))
        renderer.setPbrDebugView(static_cast<Engine::PbrDebugView>(std::atoi(debugView)));
    // GE_BISTRO_SHADOW_DEBUG=<ShadowDebugView index>, e.g. 7 residency, 4 resolved clip level.
    if (const char* shadowView = std::getenv("GE_BISTRO_SHADOW_DEBUG"))
        renderer.setShadowDebugView(static_cast<Engine::ShadowDebugView>(std::atoi(shadowView)));
    // GE_BISTRO_ADD_MODEL also adds that model here, to load the scene further.
    if (const char* model = std::getenv("GE_BISTRO_ADD_MODEL")) {
        Engine::Assets::Content content{path.parent_path().parent_path()};
        static_cast<void>(scene.createPrefab("Added", Engine::Prefab::model(content, model)));
        renderer.synchronizeScene(scene);
    }
    auto camera = scene.primaryCamera();
    ASSERT_TRUE(camera.valid());

    const std::uint32_t frames = std::getenv("GE_BISTRO_FRAMES") ? std::atoi(std::getenv("GE_BISTRO_FRAMES")) : 600;
    std::map<std::string, std::vector<float>> zones;
    std::vector<float> totals, wall, tracedProbes;
    std::vector<float> vsmNew, vsmRefreshed, vsmDirty, vsmDeferred, vsmBudget;
    int vsmInvalidatedFrames = 0, vsmScrolledFrames = 0;
    std::uint32_t vsmResident = 0;
    const char* captureDir = std::getenv("GE_BISTRO_CAPTURE_DIR");
    std::vector<SmallFrame> captured;
    std::vector<std::array<int, 3>> grids;
    // Per-frame GPU total and zones, for GE_BISTRO_FRAME_CSV.
    std::vector<std::pair<float, std::map<std::string, float>>> frameZoneHistory;
    auto previous = std::chrono::steady_clock::now();
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        // A slow walk along the street and back, looking ahead with a gentle sway.
        const float t = float(frame) / float(frames);
        const bool straight = std::getenv("GE_BISTRO_STRAIGHT") != nullptr;
        const bool still = std::getenv("GE_BISTRO_STATIC") != nullptr;
        // Straight: constant speed, fixed heading, so volume scrolls are the only discontinuity.
        const float along = straight ? -20.0F + 40.0F * t : -25.0F + 50.0F * (0.5F - 0.5F * std::cos(t * 6.2831853F));
        const float heading = straight ? 0.0F : (t < 0.5F ? -90.0F : 90.0F) + 25.0F * std::sin(t * 18.0F);
        const float side = straight ? 2.0F : 2.0F + 3.0F * std::sin(t * 9.0F);
        camera.setPosition(still ? Engine::Vec3{-8.0F, 1.8F, 2.0F} : Engine::Vec3{along, 1.8F, side});
        camera.setRotation({-4.0F, still ? -90.0F : heading, 0.0F});
        // GE_BISTRO_CAMERA="x y z pitch yaw": hold one view, e.g. to inspect a material.
        if (const char* pose = std::getenv("GE_BISTRO_CAMERA")) {
            float x = 0, y = 0, z = 0, pitch = 0, yaw = 0;
            if (std::sscanf(pose, "%f %f %f %f %f", &x, &y, &z, &pitch, &yaw) == 5) {
                camera.setPosition(Engine::Vec3{x, y, z});
                camera.setRotation({pitch, yaw, 0.0F});
            }
        }
        if (captureDir) {
            // DDGI grid cell of the camera: a change is a volume scroll.
            const float spacing = renderer.ddgiSettings().probeSpacing;
            grids.push_back({int(std::floor(along / spacing)), int(std::floor(1.8F / spacing)),
                             int(std::floor(side / spacing))});
            renderer.requestHdrCapture();
        }
        if (sceneView) {
            renderer.setSceneViewportExtent(Width, Height);
            renderer.setSceneViewportActive(true);
            renderer.setEditorCameraPosition(camera.position());
            renderer.setEditorCameraRotation(still ? -90.0F : heading, -4.0F);
            renderer.beginEditorUiFrame();
        }
        // GE_BISTRO_WATER_LATE: add an ocean on frame 5, exercising the
        // renderer's late full-size water allocation in a scene without water.
        if (frame == 5 && std::getenv("GE_BISTRO_WATER_LATE")) {
            static_cast<void>(scene.createOcean("Late ocean"));
            // A water body alone does not change the render topology.
            static_cast<void>(scene.createCube("Late ocean marker"));
            renderer.synchronizeScene(scene);
        }
        // As the Player's loop does; the render graph statistics land in the history.
        Engine::Profiler::beginFrame();
        renderer.renderFrame();
        Engine::Profiler::endFrame();
        if (captureDir) {
            std::uint32_t w = 0, h = 0;
            std::vector<std::uint16_t> pixels;
            if (renderer.takeHdrCapture(w, h, pixels))
                captured.push_back(downsample(w, h, pixels));
        }
        if (const char* report = std::getenv("GE_BISTRO_VRAM_REPORT"); report != nullptr && frame + 1 == frames) {
            std::ofstream{report} << renderer.gpuMemoryAllocationsJson();
        }
        if (frame >= 60) {
            const auto vsm = renderer.vsmStatistics();
            vsmNew.push_back(float(vsm.newPages));
            vsmRefreshed.push_back(float(vsm.refreshedPages));
            vsmDirty.push_back(float(vsm.dirtyMarkedPages));
            vsmInvalidatedFrames += vsm.levelsInvalidated != 0 ? 1 : 0;
            vsmScrolledFrames += vsm.levelsScrolled != 0 ? 1 : 0;
            vsmDeferred.push_back(float(vsm.deferredRequests));
            vsmResident = vsm.residentPages;
            vsmBudget.push_back(float(vsm.pageBudget));
        }
        const auto now = std::chrono::steady_clock::now();
        if (frame >= 60)
            wall.push_back(std::chrono::duration<float, std::milli>(now - previous).count());
        previous = now;
        const auto profile = renderer.gpuProfile();
        if (!profile || frame < 60)
            continue;
        totals.push_back(profile->frameMilliseconds);
        tracedProbes.push_back(float(renderer.ddgiStatistics().tracedProbes));
        std::map<std::string, float> frameZones;
        for (const auto& event : profile->events)
            frameZones[std::string{Engine::Profiler::name(event.name)} + (event.depth ? " (nested)" : "")] +=
                event.endMs - event.startMs;
        for (const auto& [name, ms] : frameZones)
            zones[name].push_back(ms);
        frameZoneHistory.push_back({profile->frameMilliseconds, std::move(frameZones)});
    }
    const auto median = [](std::vector<float> values) {
        if (values.empty())
            return 0.0F;
        std::ranges::sort(values);
        return values[values.size() / 2];
    };
    const auto p95 = [](std::vector<float> values) {
        if (values.empty())
            return 0.0F;
        std::ranges::sort(values);
        return values[values.size() * 95 / 100];
    };
    std::cout << std::fixed << std::setprecision(3) << "[Bistro] GPU frame median " << median(totals) << " ms, p95 "
              << p95(totals) << " ms; wall (CPU+present) median " << median(wall) << " ms, p95 " << p95(wall)
              << " ms\n";
    const auto mean = [](const std::vector<float>& values) {
        float sum = 0.0F;
        for (const float value : values)
            sum += value;
        return values.empty() ? 0.0F : sum / float(values.size());
    };
    {
        // Synchronization cost of the last recorded frame (all render graphs).
        const auto& graph =
            Engine::Profiler::historyFrame(Engine::Profiler::historySize() - 2).renderGraph;
        std::cout << "[Bistro] render graph/frame: " << graph.passes << " passes, " << graph.graphicsBatches
                  << " graphics + " << graph.computeBatches << " compute batches, " << graph.crossQueueEdges
                  << " cross-queue edges, " << graph.pipelineBarriers << " barrier calls (" << graph.imageBarriers
                  << " image, " << graph.bufferBarriers << " buffer), " << graph.ownershipTransfers
                  << " ownership transfers\n";
    }
    std::cout << "[Bistro] VSM pages/frame: new mean " << mean(vsmNew) << " p95 " << p95(vsmNew) << ", refreshed mean "
              << mean(vsmRefreshed) << " p95 " << p95(vsmRefreshed) << ", dirty-marked mean " << mean(vsmDirty)
              << ", deferred mean " << mean(vsmDeferred) << ", budget mean " << mean(vsmBudget)
              << "; frames with invalidated levels " << vsmInvalidatedFrames << ", scrolled " << vsmScrolledFrames
              << " of " << vsmNew.size() << "; resident " << vsmResident << '\n';
    if (renderer.ddgiStatistics().totalProbes != 0)
        std::cout << "[Bistro] DDGI probes traced per frame: median " << median(tracedProbes) << ", p95 "
                  << p95(tracedProbes) << " of " << renderer.ddgiStatistics().totalProbes << '\n';
    std::vector<std::pair<float, std::string>> sorted;
    for (const auto& [name, values] : zones)
        sorted.emplace_back(median(values), name);
    std::ranges::sort(sorted, std::greater{});
    for (const auto& [ms, name] : sorted)
        std::cout << "[Bistro]   " << std::setw(8) << ms << " ms  (p95 " << std::setw(7) << p95(zones[name])
                  << ")  " << name << '\n';
    // GE_BISTRO_FRAME_CSV=<path>: one row per measured frame, one column per zone.
    if (const char* csvPath = std::getenv("GE_BISTRO_FRAME_CSV")) {
        std::ofstream csv{csvPath};
        csv << "frame,gpu_ms";
        for (const auto& [name, values] : zones)
            csv << ",\"" << name << '"';
        csv << '\n';
        for (std::size_t i = 0; i < frameZoneHistory.size(); ++i) {
            const auto& [total, frameZones] = frameZoneHistory[i];
            csv << i << ',' << total;
            for (const auto& [name, values] : zones) {
                const auto found = frameZones.find(name);
                csv << ',' << (found != frameZones.end() ? found->second : 0.0F);
            }
            csv << '\n';
        }
    }
    if (captureDir && captured.size() > 2) {
        // Frame-to-frame change: camera motion gives a steady baseline,
        // lighting pops show up as spikes above it.
        std::vector<std::pair<float, std::size_t>> changes;
        for (std::size_t i = 61; i < captured.size(); ++i) {
            double sum = 0;
            for (std::size_t p = 0; p < captured[i].luminance.size(); ++p) {
                const float a = captured[i].luminance[p], b = captured[i - 1].luminance[p];
                sum += std::abs(a - b) / (0.1 + 0.5 * (a + b));
            }
            changes.emplace_back(float(sum / captured[i].luminance.size()), i);
        }
        std::vector<float> values;
        for (const auto& [change, index] : changes)
            values.push_back(change);
        std::cout << "[Bistro] frame change median " << median(values) << ", p95 " << p95(values) << '\n';
        // Captures arrive a couple of frames after their request; find the lag
        // that best aligns change with scrolls, then compare scroll frames to the rest.
        double bestRatio = 0;
        int bestLag = 0;
        for (int lag = 0; lag <= 4; ++lag) {
            double scrollSum = 0, otherSum = 0;
            int scrolls = 0, others = 0;
            for (const auto& [change, index] : changes) {
                const std::size_t request = index + std::size_t(lag);
                if (request < 1 || request >= grids.size())
                    continue;
                if (grids[request] != grids[request - 1]) {
                    scrollSum += change;
                    ++scrolls;
                } else {
                    otherSum += change;
                    ++others;
                }
            }
            if (scrolls && others && scrollSum / scrolls / (otherSum / others) > bestRatio) {
                bestRatio = scrollSum / scrolls / (otherSum / others);
                bestLag = lag;
            }
        }
        std::cout << "[Bistro] change on volume-scroll frames / other frames: " << bestRatio << " (lag " << bestLag
                  << ")\n";
        auto spikes = changes;
        std::ranges::sort(spikes, std::greater{});
        const std::filesystem::path dir{captureDir};
        std::filesystem::create_directories(dir);
        {
            std::ofstream series(dir / "changes.csv");
            series << "capture,change\n";
            for (const auto& [change, index] : changes)
                series << index << ',' << change << '\n';
            // 120x68 luminance per capture, for offline tile analysis.
            std::ofstream tiles(dir / "luminance.f32", std::ios::binary);
            for (const SmallFrame& frame : captured)
                for (int y = 0; y < 68; ++y)
                    for (int x = 0; x < 120; ++x) {
                        float value = frame.luminance[std::size_t(y * SmallFrame::Height / 68) * SmallFrame::Width +
                                                      std::size_t(x * SmallFrame::Width / 120)];
                        if (!std::isfinite(value))
                            value = 0.0F;
                        tiles.write(reinterpret_cast<const char*>(&value), sizeof(value));
                    }
            if (const char* range = std::getenv("GE_BISTRO_SAVE_RANGE")) {
                const int first = std::atoi(range), last = std::atoi(std::strchr(range, '-') + 1);
                for (int i = first; i <= last && i < int(captured.size()); ++i)
                    writePpm(dir / ("frame" + std::to_string(i) + ".ppm"), captured[std::size_t(i)]);
            }
            std::ofstream cells(dir / "grids.csv");
            cells << "frame,gx,gy,gz,captures\n";
            for (std::size_t i = 0; i < grids.size(); ++i)
                cells << i << ',' << grids[i][0] << ',' << grids[i][1] << ',' << grids[i][2] << ',' << captured.size()
                      << '\n';
        }
        for (std::size_t s = 0; s < std::min<std::size_t>(spikes.size(), 8); ++s) {
            const auto [change, index] = spikes[s];
            // Captures lag the request by the frames in flight; match the grid loosely.
            const bool scrolled = index < grids.size() && index >= 3 &&
                                  (grids[index] != grids[index - 1] || grids[index - 1] != grids[index - 2] ||
                                   grids[index - 2] != grids[index - 3]);
            std::cout << "[Bistro] spike " << s << ": frame " << index << " change " << change
                      << (scrolled ? " (DDGI volume scrolled)" : "") << '\n';
            writePpm(dir / ("spike" + std::to_string(s) + "_frame" + std::to_string(index) + ".ppm"), captured[index]);
            writePpm(dir / ("spike" + std::to_string(s) + "_frame" + std::to_string(index - 1) + "_before.ppm"),
                     captured[index - 1]);
        }
    }
    for (const auto& heap : renderer.gpuMemoryHeaps())
        std::cout << "[Bistro] heap " << heap.heapIndex << ": usage " << heap.usage / (1024 * 1024) << " MiB / budget "
                  << heap.budget / (1024 * 1024) << " MiB, engine allocations " << heap.allocationBytes / (1024 * 1024)
                  << " MiB\n";
    {
        auto allocations = renderer.gpuSceneMemory();
        std::ranges::sort(allocations, [](const auto& a, const auto& b) { return a.bytes > b.bytes; });
        for (std::size_t i = 0; i < std::min<std::size_t>(allocations.size(), 25); ++i)
            std::cout << "[Bistro] alloc " << allocations[i].table << " heap " << allocations[i].heapIndex << ": "
                      << allocations[i].bytes / 1024 << " KiB\n";
    }
    for (const auto& category : renderer.gpuMemoryCategories())
        std::cout << "[Bistro] category " << static_cast<int>(category.category) << ": "
                  << category.allocationBytes / (1024 * 1024) << " MiB\n";
    renderer.shutdown();
}
// Editor stalls on a real scene, without the editor UI: leaving Play mode
// (snapshot restore + full renderer reload) and adding a large model
// (GE_BISTRO_ADD_MODEL=<asset-root-relative model>). Prints each stage.
TEST(BistroBenchmark, EditorStalls) {
    const char* scenePath = std::getenv("GE_BISTRO_SCENE");
    if (!scenePath)
        GTEST_SKIP() << "Set GE_BISTRO_SCENE to a scene file";
    ASSERT_TRUE(SDL_InitSubSystem(SDL_INIT_VIDEO)) << SDL_GetError();
    struct Window final {
        SDL_Window* handle = SDL_CreateWindow("Editor stalls", 1920, 1080, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
        ~Window() {
            if (handle)
                SDL_DestroyWindow(handle);
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    } window;
    ASSERT_NE(window.handle, nullptr) << SDL_GetError();
    const std::filesystem::path path{scenePath};
    Engine::Scene scene;
    scene.load(path);
    Engine::Renderer renderer(Engine::RenderConfig{.features = Engine::RenderFeatures{.shadows = true}});
    renderer.setProjectRoot(path.parent_path().parent_path().parent_path());
    renderer.initialize(scene, window.handle);
    using Clock = std::chrono::steady_clock;
    const auto ms = [](Clock::time_point from) {
        return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
    };
    const auto frames = [&](int count) {
        double worst = 0;
        for (int i = 0; i < count; ++i) {
            const auto start = Clock::now();
            renderer.renderFrame();
            worst = std::max(worst, ms(start));
        }
        return worst;
    };
    frames(30);
    for (int round = 0; round < 2; ++round) {
        auto start = Clock::now();
        std::ostringstream output;
        Engine::SceneSerializer::save(scene, output);
        const std::string snapshot = std::move(output).str();
        std::cout << "[Stall] enter play: snapshot " << ms(start) << " ms (" << snapshot.size() / 1024 << " KiB)\n";
        std::cout << "[Stall]   play frames worst " << frames(20) << " ms\n";
        start = Clock::now();
        std::istringstream input{snapshot};
        Engine::SceneSerializer::load(scene, input);
        std::cout << "[Stall] exit play: restore " << ms(start) << " ms\n";
        start = Clock::now();
        renderer.reloadScene(scene, window.handle);
        std::cout << "[Stall]   reloadScene " << ms(start) << " ms\n";
        start = Clock::now();
        renderer.renderFrame();
        std::cout << "[Stall]   first frame " << ms(start) << " ms, next frames worst " << frames(20) << " ms\n";
        // The editor auto-saves 30 s later; a save must not change render topology.
        start = Clock::now();
        std::ostringstream saved;
        Engine::SceneSerializer::save(scene, saved);
        std::cout << "[Stall]   save " << ms(start) << " ms\n";
        start = Clock::now();
        renderer.synchronizeScene(scene);
        std::cout << "[Stall]   synchronizeScene after save " << ms(start) << " ms\n";
    }
    if (const char* model = std::getenv("GE_BISTRO_ADD_MODEL")) {
        Engine::Assets::Content content{path.parent_path().parent_path()};
        auto start = Clock::now();
        const auto prefab = Engine::Prefab::model(content, model);
        std::cout << "[Stall] add model: load " << ms(start) << " ms\n";
        start = Clock::now();
        static_cast<void>(scene.createPrefab("Added", prefab));
        std::cout << "[Stall]   createPrefab " << ms(start) << " ms\n";
        start = Clock::now();
        renderer.synchronizeScene(scene);
        std::cout << "[Stall]   synchronizeScene " << ms(start) << " ms\n";
        start = Clock::now();
        renderer.renderFrame();
        std::cout << "[Stall]   first frame " << ms(start) << " ms, next frames worst " << frames(20) << " ms\n";
    }
    renderer.shutdown();
}
} // namespace

