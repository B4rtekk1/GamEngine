#include "Engine/Renderer/Water/WaterRenderWorld.h"

#include "Engine/ECS/Components/MeshRendererComponent.h"
#include "Engine/ECS/Registry.h"
#include "Engine/ECS/Entity.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include "Engine/Renderer/Vulkan/SceneGpuResources.h"

namespace Engine::Water {
namespace {
[[nodiscard]] std::uint32_t spectrumRevision(const WaterBodyComponent& water) noexcept {
    // Deterministic FNV-1a over the screen-independent spectrum.  It behaves as
    // a revision key without requiring mutable bookkeeping in the ECS component.
    std::uint32_t hash = 2166136261U;
    const auto mix = [&hash](const std::uint32_t value) {
        hash ^= value;
        hash *= 16777619U;
    };
    mix(water.waveCount);
    const std::uint32_t count = std::min<std::uint32_t>(water.waveCount,
        static_cast<std::uint32_t>(water.waves.size()));
    for (std::uint32_t index = 0; index < count; ++index) {
        const WaterGerstnerWave& wave = water.waves[index];
        mix(std::bit_cast<std::uint32_t>(wave.direction.x()));
        mix(std::bit_cast<std::uint32_t>(wave.direction.y()));
        mix(std::bit_cast<std::uint32_t>(wave.amplitude));
        mix(std::bit_cast<std::uint32_t>(wave.wavelength));
        mix(std::bit_cast<std::uint32_t>(wave.speed));
        mix(std::bit_cast<std::uint32_t>(wave.steepness));
    }
    return hash == 0U ? 1U : hash;
}
} // namespace

WaterRenderWorld WaterRenderWorld::capture(const Registry& registry,
                                           const SceneGpuResources& sceneGpu) {
    WaterRenderWorld world;
    world.generation_ = registry.mutationRevision();
    // Water authoring belongs to ECS, independently of the legacy generic
    // mesh path.  Geometry bindings below are optional transitional data for
    // scenes created before VirtualWaterRenderer owns every surface buffer.
    registry.view<WaterBodyComponent>(
        [&](const Entity entity, const WaterBodyComponent& water) {
            WaterRenderBody body{};
            body.id = {entityIndex(entity), entityGeneration(entity)};
            body.water = water;
            body.spectrumRevision = spectrumRevision(water);
            // New archetypes keep geometry authoring separate from material
            // and wave settings. Legacy scenes are given a shape on load.
            if (registry.has<WaterShapeComponent>(entity)) {
                body.shape = registry.get<WaterShapeComponent>(entity);
            } else {
                body.shape.type = water.type;
                body.shape.lakePolygon = water.lakeBoundary;
                body.shape.riverSpline = water.riverSpline;
            }

            if (registry.has<MeshRendererComponent>(entity)) {
                const MeshRendererComponent& renderer = registry.get<MeshRendererComponent>(entity);
                const auto renderable = sceneGpu.renderableIndices.find(entity);
                if (renderer.hasMesh() && renderable != sceneGpu.renderableIndices.end() &&
                    !renderable->second.empty()) {
                    body.instanceIndex = static_cast<std::uint32_t>(renderable->second.front());
                    body.meshResource = renderer.mesh.resource();
                    if (const Mesh* mesh = renderer.mesh.get()) body.drawRanges = mesh->drawRanges;
                }
            }
            world.bodies_.push_back(std::move(body));
        });
    return world;
}
} // namespace Engine::Water
