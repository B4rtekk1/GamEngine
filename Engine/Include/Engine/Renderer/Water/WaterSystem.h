#pragma once

#include "Engine/ECS/Components/WaterBodyComponent.h"
#include "Engine/ECS/Registry.h"
#include "Engine/Renderer/Geometry/Mesh.h"

namespace Engine {
    class SceneEditor;
    /** Builds runtime water geometry and submits it through the Water pipeline. */
    class WaterSystem final {
    public:
        /** Rebuilds an authored lake or river, or the shared ocean clipmap. */
        void rebuild(Registry& registry, Entity entity) const;
        /** Editor-safe rebuild path; preserves Scene's Registry encapsulation. */
        void rebuild(SceneEditor& editor, Entity entity) const;

        /** Snaps every ocean's transform to the active camera in XZ. */
        void updateOceans(Registry& registry, const Vec3& cameraPosition) const;

        /** Generates CPU mesh data for editor preview and runtime submission. */
        [[nodiscard]] static Mesh buildMesh(const WaterBodyComponent& water);

        /** Internal vertex format shared by the procedural mesh builders. */
        static void addWaterVertex(Mesh& mesh, const Vec3& position, const Vec2& uv);

    private:
        [[nodiscard]] static Mesh buildOceanClipmap();
        [[nodiscard]] static Mesh buildLake(const WaterBodyComponent& water);
        [[nodiscard]] static Mesh buildRiver(const WaterBodyComponent& water);
    };
} // namespace Engine
