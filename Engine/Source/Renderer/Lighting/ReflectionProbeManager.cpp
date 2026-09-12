#include "Engine/Renderer/Lighting/ReflectionProbeManager.h"

#include "Engine/ECS/Registry.h"
#include "Engine/ECS/Components/ReflectionProbeComponent.h"
#include "Engine/ECS/Components/TransformComponent.h"
#include "Engine/Core/Transform.h"

#include <algorithm>
#include <utility>

namespace Engine {
void ReflectionProbeManager::update(const Registry& registry) {
    probes_.clear();
    slots_.clear();
    probes_.reserve(std::min<std::size_t>(registry.size(), MaxProbes));
    registry.view<Transform, ReflectionProbeComponent>([&](const Entity entity, const Transform& transform,
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
        // Each dense probe entry owns the descriptor slot with the same index.
        // The descriptor itself remains populated by the prefiltered-sky
        // fallback until the capture pass publishes its cubemap.
        gpu.textureIndex = static_cast<std::uint32_t>(probes_.size());
        slots_.insert_or_assign(entity, gpu.textureIndex);
        probes_.push_back(gpu);
    });

    // Discard handles for ECS entities removed since the previous extraction.
    for (auto it = baked_.begin(); it != baked_.end();) {
        if (!slots_.contains(*it)) it = baked_.erase(it); else ++it;
    }
}

void ReflectionProbeManager::bakeProbe(const Entity entity) {
    if (entity == NullEntity) return;
    if (std::find(bakeRequests_.begin(), bakeRequests_.end(), entity) == bakeRequests_.end())
        bakeRequests_.push_back(entity);
}

std::vector<Entity> ReflectionProbeManager::consumeBakeRequests() {
    return std::exchange(bakeRequests_, {});
}

void ReflectionProbeManager::markBaked(const Entity entity) { baked_.insert(entity); }

void ReflectionProbeManager::uploadProbeTable(const std::uint32_t) noexcept {}
} // namespace Engine
