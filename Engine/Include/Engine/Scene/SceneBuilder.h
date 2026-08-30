#pragma once

#include "Engine/Core/Transform.h"
#include "Engine/ECS/Registry.h"
#include "Engine/ECS/Components/CameraComponent.h"
#include "Engine/Renderer/MeshRenderer.h"
#include "Engine/Renderer/Materials/PBRMaterial.h"
#include "Engine/Scene/Components/LightComponent.h"
#include "Engine/Scene/Components/IdentityComponents.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

//NOLINTBEGIN
namespace Engine {
    /**
     * @brief Advanced ECS-only helper for constructing scene entities.
     *
     * SceneBuilder owns no entities or assets. It only hides the repetitive
     * Registry create/add sequence used by engine internals. Game code should
     * use Scene::createActor(), Scene::createModel() and Actor instead; that
     * API keeps Entity and Registry implementation details out of projects.
     * Include this header explicitly only for engine extensions that genuinely
     * need ECS-level entity construction.
     */
    class SceneBuilder final {
    public:
        explicit SceneBuilder(Registry &registry) noexcept
            : registry_(registry) {
        }

        [[nodiscard]] Entity createEntity(std::string name = "GameObject") const {
            const Entity entity = registry_.create();
            registry_.add<NameComponent>(entity, NameComponent{.value = std::move(name)});
            registry_.add<UUIDComponent>(entity, UUIDComponent{.value = createUUID()});
            return entity;
        }

        [[nodiscard]] Entity createMeshEntity(
            std::shared_ptr<const Mesh> mesh,
            Transform transform = {},
            const PBRMaterial &material = {},
            bool castShadow = true,
            std::uint32_t cullingBatch = 0,
            std::string name = "GameObject") const {
            const Entity entity = createEntity(std::move(name));
            registry_.add<Transform>(entity, transform);
            registry_.add<MeshRenderer>(entity, MeshRenderer{
                                            .mesh = std::move(mesh),
                                            .material = material,
                                            .castShadow = castShadow,
                                            .cullingBatch = cullingBatch,
                                        });
            return entity;
        }

        [[nodiscard]] Entity createCamera(
            Transform transform,
            CameraComponent camera = {},
            std::string name = "Camera") const {
            const Entity entity = createEntity(std::move(name));
            registry_.add<Transform>(entity, transform);
            registry_.add<CameraComponent>(entity, camera);
            return entity;
        }

        [[nodiscard]] Entity createLight(
            Transform transform,
            LightComponent light = {},
            std::string name = "Light") const {
            const Entity entity = createEntity(std::move(name));
            registry_.add<Transform>(entity, transform);
            registry_.add<LightComponent>(entity, light);
            return entity;
        }

    private:
        Registry &registry_;
    };
} // namespace Engine
//NOLINTEND
