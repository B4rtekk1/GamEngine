#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

class Camera {
public:
    Camera(float fovDegrees, float aspectRatio, float nearPlane, float farPlane);

    void setPosition(const glm::vec3& position);
    void setRotation(float yaw, float pitch);

    void move(const glm::vec3& offset);

    [[nodiscard]] glm::mat4 viewMatrix() const;
    [[nodiscard]] glm::mat4 projectionMatrix() const;

    [[nodiscard]] glm::vec3 position() const { return m_position; }
    [[nodiscard]] glm::vec3 forward() const;
    [[nodiscard]] glm::vec3 right() const;
    [[nodiscard]] glm::vec3 up() const;

    void setAspectRatio(float aspectRatio);

private:
    glm::vec3 m_position{0.0f, 0.0f, 3.0f};

    float m_yaw{-90.0f};
    float m_pitch{0.0f};

    float m_fov;
    float m_aspectRatio;
    float m_nearPlane;
    float m_farPlane;
};