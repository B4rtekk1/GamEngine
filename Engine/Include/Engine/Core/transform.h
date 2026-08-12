#pragma once

/** @file transform.h Object transform value type. */

#include "../Math/vec3.h"

namespace Engine {

/** @brief Position, Euler rotation and scale of a renderable object. */
struct Transform {
    /** @brief World-space position. */
    vec3 position{};
    /** @brief Euler rotation, in the engine's rotation units. */
    vec3 rotation{};
    /** @brief Per-axis scale; defaults to one on every axis. */
    vec3 scale{1.0f, 1.0f, 1.0f};
};

} // namespace Engine
