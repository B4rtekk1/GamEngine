#pragma once

/** @file camera.h Perspective camera interface. */

#include "math/vec3.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

/**
 * @brief Describes a perspective camera used to build view and projection matrices.
 *
 * Angles are expressed in degrees. Yaw rotates around the world Y axis and
 * pitch is clamped to prevent the camera from flipping upside down.
 */
class Camera {
public:
    /**
     * @brief Creates a camera with the specified perspective parameters.
     * @param fovDegrees Vertical field of view in degrees, in the range (0, 180).
     * @param aspectRatio Width-to-height ratio; must be positive.
     * @param nearPlane Distance to the near clipping plane.
     * @param farPlane Distance to the far clipping plane; must exceed @p nearPlane.
     * @throws std::invalid_argument If any parameter is invalid.
     */
    Camera(float fovDegrees, float aspectRatio, float nearPlane, float farPlane);

    /** @brief Sets the camera world position. */
    void setPosition(const vec3& position);
    /** @brief Sets yaw and pitch in degrees; pitch is clamped to [-89, 89]. */
    void setRotation(float yaw, float pitch);

    /** @brief Translates the camera by a world-space offset. */
    void move(const vec3& offset);

    /** @brief Returns the view matrix derived from position and orientation. */
    [[nodiscard]] glm::mat4 viewMatrix() const;
    /** @brief Returns the Vulkan-compatible perspective projection matrix. */
    [[nodiscard]] glm::mat4 projectionMatrix() const;

    /** @brief Returns the camera world position. */
    [[nodiscard]] vec3 position() const { return m_position; }
    /** @brief Returns the normalized forward direction. */
    [[nodiscard]] vec3 forward() const;
    /** @brief Returns the normalized right direction. */
    [[nodiscard]] vec3 right() const;
    /** @brief Returns the normalized up direction. */
    [[nodiscard]] vec3 up() const;

    /** @brief Updates the aspect ratio used by the projection matrix. */
    void setAspectRatio(float aspectRatio);

private:
    vec3 m_position{0.0f, 0.0f, 3.0f};

    float m_yaw{-90.0f};
    float m_pitch{0.0f};

    float m_fov;
    float m_aspectRatio;
    float m_nearPlane;
    float m_farPlane;
};
