#include "Engine/Physics/GlassDestructionSystem.h"
#include "Engine/Physics/PhysicsSystem.h"
#include "Engine/Scene/SceneEditor.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <vector>

namespace {
    using Clock = std::chrono::steady_clock;
    double milliseconds(Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now()-start).count();
    }
    void run(unsigned count, const char *state, bool single, bool burst, std::ostream &output) {
        Engine::Scene scene;
        Engine::SceneEditor editor(scene);
        Engine::PhysicsSystem physics;
        std::vector<Engine::Entity> panes;
        Engine::GlassDestructionComponent settings;
        settings.maxDynamicFragments = 8;
        const auto generationStart = Clock::now();
        for (unsigned i=0; i<count; ++i) {
            const auto pane = Engine::GlassDestructionSystem::createPane(scene, "Pane " + std::to_string(i), settings);
            pane.setPosition({float(i%25)*3, 3, float(i/25)*3});
            panes.push_back(scene.findEntity(pane.id()));
        }
        const double creationMs = milliseconds(generationStart);
        double generation = 0;
        for (auto entity : panes) generation += editor.read<Engine::GlassDestructionComponent>(entity).fractureGenerationCpuMs;
        // Record initialization separately: convex payloads must be cooked before
        // interactive impacts. These values are CPU wall times, not GPU timings.
        const auto startup = Clock::now(); physics.update(scene, 1.0F/60);
        const double warmup = milliseconds(startup);

            std::vector<double> frames;
            double impactMs = 0, physicsMs = 0;
            const auto before = physics.diagnostics();
            for (unsigned frame=0; frame<120; ++frame) {
                const auto frameStart = Clock::now();
                const auto impactStart = Clock::now();
                if (frame == 0 && (single || burst)) {
                    const unsigned affected = burst ? count : 1;
                    for (unsigned i=0; i<affected; ++i) {
                        const auto &pose = editor.read<Engine::Transform>(panes[i]);
                        (void)Engine::GlassDestructionSystem::onImpact(scene, panes[i], {.worldPosition=pose.position});
                    }
                }
                impactMs += milliseconds(impactStart);
                const auto physicsStart = Clock::now(); physics.update(scene, 1.0F/60);
                physicsMs += milliseconds(physicsStart);
                frames.push_back(milliseconds(frameStart));
                // The CPU benchmark intentionally has no renderer to consume LOD bursts.
                scene.clearGlassParticleBursts();
            }
            const auto after = physics.diagnostics();
            std::ranges::sort(frames);
            std::size_t debris = 0, sections = 0;
            editor.view<Engine::GlassDebrisComponent>([&](auto, const auto &) { ++debris; });
            editor.view<Engine::MeshRendererComponent>([&](auto, const auto &renderer) {
                sections += std::max<std::size_t>(1, renderer.mesh.resource()->metadata.sections.size());
            });
            output << count << ',' << state << ',' << generation << ',' << creationMs << ',' << warmup << ','
                << impactMs << ',' << physicsMs/frames.size() << ',' << frames[static_cast<std::size_t>(std::ceil(0.99*frames.size()))-1] << ','
                << frames.back() << ',' << after.collisionCookingCpuMs-before.collisionCookingCpuMs << ','
                << after.convexCookCount-before.convexCookCount << ',' << debris << ',' << sections << '\n';

    }
}
int main(int argc, char **argv) {
    try {
        std::ofstream file;
        if (argc > 1) file.open(argv[1]);
        if (argc > 1 && !file) throw std::runtime_error("Cannot open benchmark CSV");
        std::ostream &output = argc > 1 ? file : std::cout;
        output << "panes,state,fracture_generation_cpu_ms,creation_cpu_ms,physics_initialization_cpu_ms,impact_cpu_ms,mean_physics_cpu_ms,p99_cpu_frame_ms,peak_cpu_frame_ms,impact_cooking_cpu_ms,new_convex_cooks,active_rigid_shards,render_sections\n";
        for (unsigned count : {1U,10U,100U,500U}) {
            run(count, "intact", false, false, output);
            run(count, "single_hit", true, false, output);
            run(count, "simultaneous_hits", false, true, output);
        }
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
