#include "Editor/ComponentDescriptor.h"

#include "Engine/ECS/Components/ColliderComponent.h"
#include "Engine/ECS/Components/ProceduralCloudComponent.h"
#include "Engine/ECS/Components/RigidbodyComponent.h"
#include "Engine/ECS/Components/ScriptComponent.h"
#include "Engine/ECS/Components/SmokeEmitterComponent.h"
#include "Engine/ECS/Components/TerrainComponent.h"
#include "Engine/ECS/Components/WindComponent.h"
#include "Engine/Renderer/MeshRenderer.h"
#include "Engine/Renderer/Geometry/ProceduralCloud.h"
#include "Engine/Scene/Components/LightComponent.h"
#include "Engine/Scene/SceneEditor.h"

#include <algorithm>
#include <utility>

namespace Editor {
    ComponentRegistry& ComponentRegistry::instance() {
        static ComponentRegistry registry;
        return registry;
    }

    void ComponentRegistry::registerComponent(ComponentDescriptor descriptor) {
        const auto existing = std::ranges::find(components_, descriptor.name, &ComponentDescriptor::name);
        if (existing == components_.end()) {
            components_.push_back(std::move(descriptor));
        } else {
            *existing = std::move(descriptor);
        }
        std::ranges::sort(components_, {}, [](const ComponentDescriptor& item) {
            return std::pair{item.category, item.name};
        });
    }

    const std::vector<ComponentDescriptor>& ComponentRegistry::components() const noexcept {
        return components_;
    }

    void registerBuiltinComponents() {
        static const bool registered = [] {
            auto& registry = ComponentRegistry::instance();
            const auto standardCanAdd = [](auto componentTag) {
                using Component = decltype(componentTag);
                return [](Engine::ScenePreset& scene, const Engine::Entity entity) {
                    return scene.editor().valid(entity) && !scene.editor().has<Component>(entity);
                };
            };
            const auto standardAdd = [](auto componentTag) {
                using Component = decltype(componentTag);
                return [](Engine::ScenePreset& scene, const Engine::Entity entity) {
                    scene.editor().add<Component>(entity);
                };
            };
            const auto standardRemove = [](auto componentTag) {
                using Component = decltype(componentTag);
                return [](Engine::ScenePreset& scene, const Engine::Entity entity) {
                    scene.editor().remove<Component>(entity);
                };
            };
            const auto registerStandard = [&](std::string_view name, std::string_view category,
                                              std::string_view description, auto componentTag) {
                registry.registerComponent({name, category, description, false, true,
                    standardCanAdd(componentTag), standardAdd(componentTag), standardRemove(componentTag), {}});
            };

            registerStandard("Collider", "Physics", "Adds PhysX collision geometry.", Engine::ColliderComponent{});
            registerStandard("Rigidbody", "Physics", "Adds PhysX rigid-body simulation.", Engine::RigidbodyComponent{});
            registerStandard("Script", "Scripting", "Attaches a registered C++ behaviour.", Engine::ScriptComponent{});
            registerStandard("Smoke Emitter", "Effects", "Emits GPU-simulated smoke particles.", Engine::SmokeEmitterComponent{});
            registerStandard("Light", "Rendering", "Adds a directional, point, or spot light.", Engine::LightComponent{});
            registerStandard("Mesh Renderer", "Rendering", "Renders a mesh using a PBR material.", Engine::MeshRenderer{});
            registry.registerComponent({
                "Procedural Cloud", "Effects", "Generates a deterministic puff cloud mesh.", false, true,
                standardCanAdd(Engine::ProceduralCloudComponent{}),
                [](Engine::ScenePreset& scene, const Engine::Entity entity) {
                    Engine::ProceduralCloudComponent cloud;
                    if (!scene.editor().has<Engine::MeshRenderer>(entity)) {
                        scene.editor().add<Engine::MeshRenderer>(entity);
                    }
                    scene.editor().add<Engine::ProceduralCloudComponent>(entity, cloud);
                    scene.editor().patch<Engine::MeshRenderer>(entity, [&](auto& renderer) {
                        renderer.mesh = std::make_shared<Engine::Mesh>(Engine::ProceduralCloud::createMesh(cloud));
                    });
                },
                [](Engine::ScenePreset& scene, const Engine::Entity entity) {
                    // The cloud owns the generated mesh attached to the
                    // MeshRenderer. Removing only the marker component leaves
                    // that mesh renderable, making the cloud appear to remain.
                    if (scene.editor().has<Engine::ProceduralCloudComponent>(entity)) {
                        scene.editor().remove<Engine::ProceduralCloudComponent>(entity);
                    }
                    if (scene.editor().has<Engine::MeshRenderer>(entity)) {
                        scene.editor().remove<Engine::MeshRenderer>(entity);
                    }
                }, {}});
            registerStandard("Terrain", "Environment", "Stores editable terrain data.", Engine::TerrainComponent{});
            registry.registerComponent({
                "Wind", "Environment", "Provides the scene-wide wind source.", true, true,
                [](Engine::ScenePreset& scene, Engine::Entity entity) {
                    if (!scene.editor().valid(entity) || scene.editor().has<Engine::WindComponent>(entity)) return false;
                    bool exists = false;
                    scene.editor().view<Engine::WindComponent>([&](Engine::Entity, const auto&) { exists = true; });
                    return !exists;
                },
                [](Engine::ScenePreset& scene, Engine::Entity entity) { scene.editor().add<Engine::WindComponent>(entity); },
                [](Engine::ScenePreset& scene, Engine::Entity entity) { scene.editor().remove<Engine::WindComponent>(entity); },
                {}});
            return true;
        }();
        static_cast<void>(registered);
    }
} // namespace Editor
