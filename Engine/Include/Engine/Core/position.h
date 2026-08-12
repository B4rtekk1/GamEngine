#pragma once

/** @file position.h Position value type. */

#include "../Math/vec3.h"

namespace Engine {

/** @brief Position of an object in three-dimensional world space. */
struct Position3 {
    /** @brief Underlying Cartesian coordinates. */
    vec3 value{};

    /** @brief Constructs a position at the origin. */
    Position3() = default;

    /** @brief Constructs a position from Cartesian coordinates. */
    Position3(const float x, const float y, const float z) : value(x, y, z) {}

    /** @brief Constructs a position from a vector. */
    explicit Position3(const vec3& v) : value(v) {}
};

} // namespace Engine
