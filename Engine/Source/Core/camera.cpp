/** @file camera.cpp Camera implementation. */

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include "Engine/Core/camera.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Engine {

namespace {
    constexpr float MIN_PITCH = -89.0f;
    constexpr float MAX_PITCH = 89.0f;
    constexpr vec3 WORLD_UP{0.0f, 1.0f, 0.0f};
}

Camera::Camera(const float fovDegrees, const float aspectRatio, const float nearPlane, const float farPlane)
    : m_fov(fovDegrees),
      m_aspectRatio(aspectRatio),
      m_nearPlane(nearPlane),
      m_farPlane(farPlane) {
    if (fovDegrees <= 0.0f || fovDegrees >= 180.0f) {
        throw std::invalid_argument("Camera field of view must be between 0 and 180 degrees");
    }
    if (aspectRatio <= 0.0f) {
        throw std::invalid_argument("Camera aspect ratio must be greater than zero");
    }
    if (nearPlane <= 0.0f || farPlane <= nearPlane) {
        throw std::invalid_argument("Camera clipping planes are invalid");
    }
}

void Camera::setPosition(const vec3& position) {
    m_position = position;
}

void Camera::setRotation(const float yaw, const float pitch) {
    m_yaw = yaw;
    m_pitch = std::clamp(pitch, MIN_PITCH, MAX_PITCH);
}

void Camera::move(const vec3& offset) {
    m_position += offset;
}

glm::mat4 Camera::viewMatrix() const {
    return glm::lookAt(m_position.native(), (m_position + forward()).native(), up().native());
}

glm::mat4 Camera::projectionMatrix() const {
    glm::mat4 projection = glm::perspective(
        glm::radians(m_fov), m_aspectRatio, m_nearPlane, m_farPlane);

    // Vulkan's viewport has its Y axis pointing down.
    projection[1][1] *= -1.0f;
    return projection;
}

vec3 Camera::forward() const {
    const float yaw = glm::radians(m_yaw);
    const float pitch = glm::radians(m_pitch);

    return vec3{
        std::cos(pitch) * std::cos(yaw),
        std::sin(pitch),
        std::cos(pitch) * std::sin(yaw)
    }.normalized();
}

vec3 Camera::right() const {
    return vec3{glm::normalize(glm::cross(forward().native(), WORLD_UP.native()))};
}

vec3 Camera::up() const {
    return vec3{glm::normalize(glm::cross(right().native(), forward().native()))};
}

void Camera::setAspectRatio(const float aspectRatio) {
    if (aspectRatio <= 0.0f) {
        throw std::invalid_argument("Camera aspect ratio must be greater than zero");
    }
    m_aspectRatio = aspectRatio;
}

} // namespace Engine
