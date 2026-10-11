#pragma once

#include "Engine/ECS/Entity.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>
#include <memory>

namespace Engine {
    class Mesh;
    enum class GlassType : std::uint8_t { Annealed, Tempered, Laminated, Armored };
    enum class GlassSupportMode : std::uint8_t { Bottom, Frame, Custom };
    enum class GlassRenderState : std::uint8_t { Intact, Cracked, Fractured };
    struct GlassCrack final {
        glm::vec2 position{};
        float radius{}; // Local radius, independent of physical fragmentation.
        float intensity{};
        std::uint32_t seed{};
    };
    struct GlassParticleBurst final {
        glm::vec3 position{}, velocity{};
        float size{0.02F}, lifetime{2.0F};
        std::uint32_t count{1}, seed{};
    };
    struct GlassSimulationSettings final {
        std::uint32_t maxDynamicFragmentsGlobal{256};
        float minDynamicFragmentArea{0.0004F};
        float kineticEnergyFraction{0.08F};
        float maxLinearSpeed{15.0F};
        bool adaptivePhysicsBudget{true};
        float targetPhysicsCpuMs{2.0F};
        float maxAngularSpeed{30.0F}; // radians per second
    };
    struct GlassSupportReference final {
        Entity entity{NullEntity};
        std::uint64_t worldRevision{};
        glm::mat4 worldMatrix{1};
    };
    struct GlassFragment final {
        std::vector<glm::vec2> polygon;
        glm::vec2 center{};
        float area{};
        float damage{};
        bool attached{true};
        bool frameSupported{};
        float anchorDamage{};
        std::vector<float> layerDamage; // Local armored layer energy reservoirs.
        Entity debrisEntity{NullEntity}; // Runtime GPU slot owner; rebuilt after loading.
        mutable std::shared_ptr<const Mesh> collisionGeometry; // Centered, immutable shard geometry.
    };

    struct GlassBond final {
        std::uint32_t first{}, second{};
        glm::vec2 center{};
        float damage{}; // Normalized legacy/save inspection value.
        glm::vec2 edgeStart{}, edgeEnd{};
        float length{};
        float strength{1.0F}; // Gameplay fracture energy budget.
        float accumulatedDamage{};
        bool broken{};
    };

    /** Rectangular pane in local XY, centered at the origin. Runtime data is scene-owned. */
    struct GlassDestructionComponent final {
        glm::vec2 size{2.0F, 2.0F};
        float thickness{0.006F};
        float fractureThreshold{20.0F};
        float density{2500.0F};
        std::uint32_t maxFragments{96};
        std::uint32_t maxDynamicFragments{64};
        std::uint32_t seed{1};
        float fragmentLifetime{4.0F};
        GlassType type{GlassType::Annealed};
        GlassSupportMode supportMode{GlassSupportMode::Frame};
        std::uint8_t supportEdges{15}; // Custom: bottom=1, right=2, top=4, left=8.
        std::uint32_t layers{3}; // Armored damage reservoirs.
        float bondStrength{1.0F};
        float crackWidth{0.002F};
        float accumulatedEnergy{};
        GlassRenderState renderState{GlassRenderState::Intact};
        std::vector<GlassCrack> cracks;
        std::uint32_t impactCount{};
        mutable std::uint32_t crackAtlasIndex{~std::uint32_t{0}}; // Renderer-owned runtime slot.
        bool fractured{};
        bool fullyDestroyed{};
        /// Once a scene collider supports the bottom, losing it releases the pane.
        bool hadPhysicalSupport{};
        std::vector<GlassFragment> fragments;
        std::vector<GlassBond> bonds;
        mutable std::shared_ptr<Mesh> fractureRenderGeometry;
        mutable std::shared_ptr<Mesh> intactRenderGeometry;
        std::shared_ptr<const GlassDestructionComponent> fractureTemplate; // Immutable geometry pool owner.
        // Runtime support observation is invalidated by collider/transform changes.
        std::uint64_t supportColliderRevision{~std::uint64_t{0}};
        std::uint64_t supportTransformRevision{~std::uint64_t{0}};
        std::uint64_t supportStructureRevision{~std::uint64_t{0}};
        glm::vec3 supportGravity{};
        std::uint64_t supportPaneWorldRevision{};
        glm::mat4 supportPaneMatrix{1};
        std::vector<GlassSupportReference> physicalSupports;
        std::uint64_t supportRaycasts{};
        double fractureGenerationCpuMs{}, bondSolverCpuMs{}, debrisSpawnCpuMs{};
    };

    /** Physics-only actor driving an existing pane render section. */
    struct GlassFragmentBindingComponent final {
        std::uint64_t paneUuid{};
        std::uint32_t fragmentIndex{};
        Entity paneEntity{NullEntity};
    };

    /** Runtime marker for automatic debris cleanup. */
    struct GlassDebrisComponent final {
        float remainingLifetime{4.0F};
    };
}
