#pragma once

#include <glm/glm.hpp>

class Vec3 {
    public:
    constexpr Vec3() = default;

    constexpr Vec3(const float x, const float y, const float z) : m_value(x, y, z) {}

    explicit constexpr Vec3(const glm::vec3& value) : m_value(value) {}

    [[nodiscard]] constexpr float x() const noexcept { return m_value.x; }
    [[nodiscard]] constexpr float y() const noexcept { return m_value.y; }
    [[nodiscard]] constexpr float z() const noexcept { return m_value.z; }

    [[nodiscard]] constexpr glm::vec3 toGlm() const noexcept { return m_value; }

    constexpr void setX(const float x) noexcept { m_value.x = x; }
    constexpr void setY(const float y) noexcept { m_value.y = y; }
    constexpr void setZ(const float z) noexcept { m_value.z = z; }

    constexpr Vec3 operator+(const Vec3& rhs) const noexcept {return Vec3{m_value + rhs.m_value}; }
    constexpr Vec3 operator-(const Vec3& rhs) const noexcept {return Vec3{m_value - rhs.m_value}; }
    constexpr Vec3 operator-() const noexcept { return Vec3{-m_value}; }
    constexpr Vec3 operator*(const Vec3& rhs) const noexcept {return Vec3{m_value * rhs.m_value}; }
    constexpr Vec3 operator*(const float scalar) const noexcept { return Vec3{m_value * scalar}; }
    constexpr Vec3& operator+=(const Vec3& rhs) noexcept { *this = *this + rhs; return *this; }
    constexpr Vec3& operator-=(const Vec3& rhs) noexcept { *this = *this - rhs; return *this; }
    constexpr Vec3& operator*=(const Vec3& rhs) noexcept { *this = *this * rhs; return *this; }
    constexpr Vec3& operator*=(const float scalar) noexcept { *this = *this * scalar; return *this; }

    [[nodiscard]] float length() const noexcept { return glm::length(m_value); }
    [[nodiscard]] Vec3 normalized() const noexcept { return Vec3{glm::normalize(m_value)}; }
    [[nodiscard]] glm::vec3 native() const noexcept { return m_value; }

private:
    glm::vec3 m_value;
};

constexpr Vec3 operator*(const float scalar, const Vec3& value) noexcept { return value * scalar; }
