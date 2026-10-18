#pragma once

#include "Engine/Core/Camera.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/Scene/Components/IdentityComponents.h"
#include "Engine/Scene/TransformSystem.h"

namespace Engine {
struct SceneCameraPose final {
    Vec3 position;
    float yaw{};
    float pitch{};
};

/** Matches the renderer's view-angle convention, including parent transforms.
 * Call TransformSystem::updateDirty before reading the world position. */
[[nodiscard]] inline SceneCameraPose sceneCameraPose(const Registry& registry, const Entity entity) {
    const auto& transform = registry.get<Transform>(entity);
    SceneCameraPose pose{transform.worldPosition(), transform.rotation.y(), transform.rotation.x()};
    Entity current = entity;
    for (std::size_t depth = 0; depth < registry.entityIndexCapacity(); ++depth) {
        if (!registry.has<ParentComponent>(current))
            break;
        current = registry.get<ParentComponent>(current).runtimeParent;
        if (!registry.valid(current) || !registry.has<Transform>(current) || current == entity)
            break;
        const auto& parent = registry.get<Transform>(current);
        pose.yaw += parent.rotation.y();
        pose.pitch += parent.rotation.x();
    }
    return pose;
}

/** Camera shared by gameplay rays and debug projection. Zero keeps authored aspect. */
[[nodiscard]] inline Camera makeSceneCamera(Registry& registry, const Entity entity,
                                            const float aspectOverride = 0.0F) {
    TransformSystem::updateDirty(registry);
    const auto& settings = registry.get<CameraComponent>(entity);
    Camera camera{Degrees{settings.fieldOfView}, aspectOverride > 0.0F ? aspectOverride : settings.aspectRatio,
                  settings.nearClip, settings.farClip};
    const auto pose = sceneCameraPose(registry, entity);
    camera.setPosition(pose.position);
    camera.setRotation(Degrees{pose.yaw}, Degrees{pose.pitch});
    return camera;
}
} // namespace Engine
