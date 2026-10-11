#pragma once

#include "Engine/ECS/Entity.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace Engine {
    struct GlassFragment final {
        std::vector<glm::vec2> polygon;
        glm::vec2 center{};
        float area{};
        float damage{};
        bool attached{true};
        bool frameSupported{};
    };

    struct GlassBond final {
        std::uint32_t first{}, second{};
        glm::vec2 center{};
        float damage{};
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
        bool fractured{};
        bool fullyDestroyed{};
        /// Once a scene collider supports the bottom, losing it releases the pane.
        bool hadPhysicalSupport{};
        std::vector<GlassFragment> fragments;
        std::vector<GlassBond> bonds;
    };

    /** Runtime marker for automatic debris cleanup. */
    struct GlassDebrisComponent final {
        float remainingLifetime{4.0F};
    };
}
