#pragma once

#include "Vec3.h"

struct Position3 {
    Vec3 value{};

    Position3() = default;

    Position3(float x, float y, float z) : value(x, y, z) {}

    explicit Position3(const Vec3& v) : value(v) {}
};
