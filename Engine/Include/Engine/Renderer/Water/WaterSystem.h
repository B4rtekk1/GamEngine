#pragma once

#include "Engine/ECS/Components/WaterBodyComponent.h"
#include "Engine/ECS/Registry.h"
#include "Engine/Renderer/Geometry/Mesh.h"

#include <optional>

namespace Engine {
    class SceneEditor;

    struct WaterQueryResult final {
        float surfaceHeight{};
        Vec3 normal{0.0F, 1.0F, 0.0F};
        Vec3 velocity{};
        float depth{};
        float immersion{};
        float flowSpeed{};
        Entity waterBody{NullEntity};
        WaterBodyType type{WaterBodyType::Ocean};
    };
    /** Builds runtime water geometry and submits it through the Water pipeline. */
    class WaterSystem final {
    public:
        /** Rebuilds an authored lake or river, or the shared ocean clipmap. */
        void rebuild(Registry& registry, Entity entity) const;
        /** Editor-safe rebuild path; preserves Scene's Registry encapsulation. */
        void rebuild(SceneEditor& editor, Entity entity) const;

        /** Snaps every ocean's transform to the active camera in XZ. */
        void updateOceans(Registry& registry, const Vec3& cameraPosition) const;

        /**
         * Analytic gameplay/physics query independent of render-page residency.
         * Returns the highest matching water surface at @p worldPosition.
         */
        [[nodiscard]] static std::optional<WaterQueryResult> query(
            Registry& registry, const Vec3& worldPosition, float time);

        /** Generates CPU mesh data for editor preview and runtime submission. */
        [[nodiscard]] static Mesh buildMesh(const WaterBodyComponent& water);

        /** Internal vertex format shared by the procedural mesh builders. */
        // `cellSize` is carried in TEXCOORD_1.x for the Water shader.  A
        // non-positive value denotes authored (non-clipmap) water.
        static void addWaterVertex(Mesh& mesh, const Vec3& position, const Vec2& uv,
                                   float cellSize = 0.0F);

    private:
        [[nodiscard]] static Mesh buildOceanClipmap();
        [[nodiscard]] static Mesh buildLake(const WaterBodyComponent& water);
        [[nodiscard]] static Mesh buildRiver(const WaterBodyComponent& water);
    };
} // namespace Engine
