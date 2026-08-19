#pragma once

#include "Engine/Core/Transform.h"
#include "Engine/ECS/Registry.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/Renderer/MeshRenderer.h"
#include "Engine/Renderer/Materials/PBRMaterial.h"
#include "Engine/Scene/Components/LightComponent.h"

#include <cstdint>
#include <memory>
#include <utility>

namespace Engine {

/**
 * @brief Small, fluent-friendly helper for constructing ECS scene entities.
 *
 * SceneBuilder owns no entities or assets. It only hides the repetitive
 * Registry create/add sequence used when assembling a scene.
 */
class SceneBuilder final {
public:
    explicit SceneBuilder(Registry& registry) noexcept
        : registry_(registry) {}

    [[nodiscard]] Entity createEntity() {
        return registry_.create();
    }

    [[nodiscard]] Entity createMeshEntity(
        std::shared_ptr<const Mesh> mesh,
        Transform transform = {},
        PBRMaterial material = {},
        bool castShadow = true,
        std::uint32_t cullingBatch = 0) {
        const Entity entity = createEntity();
        registry_.add<Transform>(entity, std::move(transform));
        registry_.add<MeshRenderer>(entity, MeshRenderer{
            .mesh = std::move(mesh),
            .material = std::move(material),
            .castShadow = castShadow,
            .cullingBatch = cullingBatch,
        });
        return entity;
    }

    [[nodiscard]] Entity createCamera(
        Transform transform,
        CameraComponent camera = {}) {
        const Entity entity = createEntity();
        registry_.add<Transform>(entity, std::move(transform));
        registry_.add<CameraComponent>(entity, std::move(camera));
        return entity;
    }

    [[nodiscard]] Entity createLight(
        Transform transform,
        LightComponent light = {}) {
        const Entity entity = createEntity();
        registry_.add<Transform>(entity, std::move(transform));
        registry_.add<LightComponent>(entity, std::move(light));
        return entity;
    }

private:
    Registry& registry_;
};

} // namespace Engine
