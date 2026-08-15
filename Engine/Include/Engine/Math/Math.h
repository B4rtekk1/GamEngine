#pragma once

#include "AABB.h"
#include "Angle.h"
#include "Color.h"
#include "Degrees.h"
#include "Mat4.h"
#include "Quat.h"
#include "Radians.h"
#include "Vec2.h"
#include "Vec3.h"
#include "Vec4.h"

namespace Engine {

/** @brief Returns the scalar product of two three-dimensional vectors. */
template <typename T>
[[nodiscard]] constexpr typename T::value_type dot(const T &lhs, const T &rhs) noexcept {
    return lhs.x() * rhs.x() + lhs.y() * rhs.y() + lhs.z() * rhs.z();
}

} // namespace Engine

#include "Engine/Math/Frustum.h"
