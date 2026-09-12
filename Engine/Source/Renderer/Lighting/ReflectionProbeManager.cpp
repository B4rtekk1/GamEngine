#include "Engine/Renderer/Lighting/ReflectionProbeManager.h"

#include "Engine/ECS/Registry.h"
#include "Engine/ECS/Components/ReflectionProbeComponent.h"
#include "Engine/ECS/Components/TransformComponent.h"
#include "Engine/Core/Transform.h"

#include <algorithm>

namespace Engine {
void ReflectionProbeManager::update(const Registry& registry) {
    probes_.clear();
    probes_.reserve(std::min<std::size_t>(registry.size(), MaxProbes));
    registry.view<Transform, ReflectionProbeComponent>([&](const Entity, const Transform& transform,
                                                            const ReflectionProbeComponent& probe) {
        if (probes_.size() == MaxProbes) return;
        const glm::vec3 position = glm::vec3(transform.worldMatrix().native()[3]);
        GpuReflectionProbe gpu{};
        gpu.positionAndBlendDistance = {position, std::max(probe.blendDistance, 1.0e-4F)};
        gpu.extentsAndShape = {probe.extents.x(), probe.extents.y(), probe.extents.z(),
                               probe.shape == ReflectionProbeShape::Box ? 0.0F : 1.0F};
        gpu.priority = probe.priority;
        gpu.flags = (probe.shape == ReflectionProbeShape::Box ? Probe_Box : 0u) |
                    (probe.boxProjection ? Probe_BoxProject : 0u) |
                    (probe.enabled ? Probe_Enabled : 0u);
        // Slot zero is the valid prefiltered-environment fallback until bakeProbe
        // attaches a captured cubemap to this entry.
        gpu.textureIndex = 0;
        probes_.push_back(gpu);
    });
}

void ReflectionProbeManager::bakeProbe(const Entity) {
    // Capture rendering is deliberately queued by the Vulkan backend. Keeping
    // selection/table construction independent means edits never require CPU
    // per-renderable probe selection.
}

void ReflectionProbeManager::uploadProbeTable(const std::uint32_t) noexcept {}
} // namespace Engine
