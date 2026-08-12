#pragma once

/** @file position.h Position value type. */

#include "Vec3.h"

/** @brief Position of an object in three-dimensional world space. */
struct Position3 {
    /** @brief Underlying Cartesian coordinates. */
    Vec3 value{};

    /** @brief Constructs a position at the origin. */
    Position3() = default;

    /** @brief Constructs a position from Cartesian coordinates. */
    Position3(float x, float y, float z) : value(x, y, z) {}

    /** @brief Constructs a position from a vector. */
    explicit Position3(const Vec3& v) : value(v) {}
};
