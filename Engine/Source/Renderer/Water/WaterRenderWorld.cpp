#include "Engine/Renderer/Water/WaterRenderWorld.h"

#include "Engine/ECS/Components/MeshRendererComponent.h"
#include "Engine/ECS/Registry.h"
#include "Engine/Renderer/Vulkan/SceneGpuResources.h"

namespace Engine::Water {
WaterRenderWorld WaterRenderWorld::capture(const Registry& registry,
                                           const SceneGpuResources& sceneGpu) {
    WaterRenderWorld world;
    // Water authoring belongs to ECS, independently of the legacy generic
    // mesh path.  Geometry bindings below are optional transitional data for
    // scenes created before VirtualWaterRenderer owns every surface buffer.
    registry.view<WaterBodyComponent>(
        [&](const Entity entity, const WaterBodyComponent& water) {
            WaterRenderBody body{};
            body.water = water;
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
