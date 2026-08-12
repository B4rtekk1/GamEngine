#pragma once

/** @file transform.h Object transform value type. */

#include "Vec3.h"

/** @brief Position, Euler rotation and scale of a renderable object. */
struct Transform {
    /** @brief World-space position. */
    Vec3 position{};
    /** @brief Euler rotation, in the engine's rotation units. */
    Vec3 rotation{};
    /** @brief Per-axis scale; defaults to one on every axis. */
    Vec3 scale{1.0f, 1.0f, 1.0f};
};
