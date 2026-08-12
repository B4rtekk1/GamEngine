#pragma once

#include "Vec3.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

class Camera {
public:
    Camera(float fovDegrees, float aspectRatio, float nearPlane, float farPlane);

    void setPosition(const Vec3& position);
    void setRotation(float yaw, float pitch);

    void move(const Vec3& offset);

    [[nodiscard]] glm::mat4 viewMatrix() const;
    [[nodiscard]] glm::mat4 projectionMatrix() const;

    [[nodiscard]] Vec3 position() const { return m_position; }
    [[nodiscard]] Vec3 forward() const;
    [[nodiscard]] Vec3 right() const;
    [[nodiscard]] Vec3 up() const;

    void setAspectRatio(float aspectRatio);

private:
    Vec3 m_position{0.0f, 0.0f, 3.0f};

    float m_yaw{-90.0f};
    float m_pitch{0.0f};

    float m_fov;
    float m_aspectRatio;
    float m_nearPlane;
    float m_farPlane;
};
