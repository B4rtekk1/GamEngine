#pragma once

#include "Engine/ECS/Actor.h"
#include "Engine/ECS/Components/GlassDestructionComponent.h"
#include <memory>

namespace Engine {
    class Scene;
    class Mesh;
    class PhysicsSystem;
    struct GlassImpact final {
        Vec3 worldPosition;
        Vec3 worldDirection{0.0F, 0.0F, -1.0F};
        float energy{40.0F};
        /// Radius measured in world units.
        float radius{0.18F};
    };

    class GlassDestructionSystem final {
    public:
        /** Creates an opt-in destructible rectangular pane; does not classify imported meshes. */
        [[nodiscard]] static Actor createPane(Scene &scene, std::string name,
            GlassDestructionComponent settings = {}, PBRMaterial material = {});
        /** Returns false for invalid, below-threshold or off-surface impacts. */
        static bool onImpact(Scene &scene, Entity entity, const GlassImpact &impact);
        static void update(Scene &scene, float deltaTime, const PhysicsSystem *physics = nullptr);
        /** Stable packed fragment sections, used by GPU indirect rendering and scene loading. */
        [[nodiscard]] static std::shared_ptr<Mesh> renderMesh(const GlassDestructionComponent &glass);
        [[nodiscard]] static std::shared_ptr<Mesh> collisionMesh(const GlassDestructionComponent &glass);
    };
}
