#include "Engine/Renderer/Water/WaterRenderWorld.h"

#include "Engine/ECS/Components/MeshRendererComponent.h"
#include "Engine/ECS/Registry.h"
#include "Engine/Renderer/Vulkan/SceneGpuResources.h"

namespace Engine::Water {
WaterRenderWorld WaterRenderWorld::capture(const Registry& registry,
                                           const SceneGpuResources& sceneGpu) {
    WaterRenderWorld world;
    registry.view<WaterBodyComponent, MeshRendererComponent>(
        [&](const Entity entity, const WaterBodyComponent& water, const MeshRendererComponent& renderer) {
            if (!renderer.hasMesh()) return;
            const auto renderable = sceneGpu.renderableIndices.find(entity);
            if (renderable == sceneGpu.renderableIndices.end() || renderable->second.empty()) return;

            WaterRenderBody body{};
            body.water = water;
            body.instanceIndex = static_cast<std::uint32_t>(renderable->second.front());
            body.meshResource = renderer.mesh.resource();
            if (const Mesh* mesh = renderer.mesh.get()) {
                body.drawRanges = mesh->drawRanges;
            }
            world.bodies_.push_back(std::move(body));
        });
    return world;
}
} // namespace Engine::Water
