#pragma once
//NOLINTBEGIN
#include "AABB.h"
#include "Angle.h"
#include "Color.h"
#include "Frustum.h"
#include "Mat4.h"
#include "Quat.h"
#include "Trigonometry.h"
#include "Vec2.h"
#include "Vec3.h"
#include "Vec4.h"

namespace Engine {

    /** @brief Mathematical constant π as a single-precision value. */
    inline constexpr float Pi = 3.14159265358979323846F;

    /** @brief Mathematical constant 2π as a single-precision value. */
    inline constexpr float Tau = 6.28318530717958647692F;

    /** @brief Half of π. */
    inline constexpr float HalfPi = 1.57079632679489661923F;

/** @brief Returns the scalar product of two three-dimensional vectors. */
template <typename T>
[[nodiscard]] constexpr T::value_type dot(const T &lhs, const T &rhs) noexcept {
    return lhs.x() * rhs.x() + lhs.y() * rhs.y() + lhs.z() * rhs.z();
}

} // namespace Engine
//NOLINTEND