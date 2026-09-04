/** @file camera.cpp Camera implementation. */

#include "Engine/Core/Camera.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

// NOLINTBEGIN(readability-magic-numbers)

namespace Engine {

namespace {
    constexpr float MIN_PITCH = -89.0F;
    constexpr float MAX_PITCH = 89.0F;
    constexpr Vec3 WORLD_UP{0.0F, 1.0F, 0.0F};
}

Camera::Camera(const Degrees fov, const float aspectRatio, const float nearPlane, const float farPlane)
    : m_fov(fov),
      m_aspectRatio(aspectRatio),
      m_nearPlane(nearPlane),
      m_farPlane(farPlane) {
    if (fov.value() <= 0.0F || fov.value() >= 180.0F) {
        throw std::invalid_argument("Camera field of view must be between 0 and 180 degrees");
    }
    if (aspectRatio <= 0.0F) {
        throw std::invalid_argument("Camera aspect ratio must be greater than zero");
    }
    if (nearPlane <= 0.0F || farPlane <= nearPlane) {
        throw std::invalid_argument("Camera clipping planes are invalid");
    }
}

void Camera::setPosition(const Vec3& position) {
    m_position = position;
}

void Camera::setRotation(const Degrees yaw, const Degrees pitch, const Degrees roll) {
    m_yaw = yaw;
    m_pitch = Degrees{std::clamp(pitch.value(), MIN_PITCH, MAX_PITCH)};
    m_roll = roll;
}

void Camera::move(const Vec3& offset) {
    m_position += offset;
}

Mat4 Camera::viewMatrix() const {
    return Mat4::lookAt(m_position, m_position + forward(), up());
}

Mat4 Camera::projectionMatrix() const {
    Mat4 projection = unjitteredProjectionMatrix();
    projection.native()[2][0] += m_jitterX;
    projection.native()[2][1] += m_jitterY;
    return projection;
}

Mat4 Camera::unjitteredProjectionMatrix() const {
    Mat4 projection = Mat4::perspective(
        Radians{m_fov},
        m_aspectRatio,
        m_nearPlane,
        m_farPlane
    );

    // Vulkan's viewport has its Y axis pointing down.
    projection.native()[1][1] *= -1.0F;
    return projection;
}

Vec3 Camera::forward() const {
    const float yaw = Radians{m_yaw}.value();
    const float pitch = Radians{m_pitch}.value();

    return Vec3{
        std::cos(pitch) * std::cos(yaw),
        std::sin(pitch),
        std::cos(pitch) * std::sin(yaw)
    }.normalized();
}

Vec3 Camera::right() const {
    const Vec3 unrolledRight = cross(forward(), WORLD_UP).normalized();
    const Vec3 unrolledUp = cross(unrolledRight, forward()).normalized();
    const float roll = Radians{m_roll}.value();
    return (unrolledRight * std::cos(roll) + unrolledUp * std::sin(roll)).normalized();
}

Vec3 Camera::up() const {
    return cross(right(), forward()).normalized();
}

void Camera::setAspectRatio(const float aspectRatio) {
    if (aspectRatio <= 0.0F) {
        throw std::invalid_argument("Camera aspect ratio must be greater than zero");
    }
    m_aspectRatio = aspectRatio;
}

void Camera::setProjectionJitter(const float x, const float y) noexcept {
    m_jitterX = x;
    m_jitterY = y;
}

} // namespace Engine

// NOLINTEND(readability-magic-numbers)
