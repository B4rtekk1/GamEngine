#pragma once

/** @file camera.h Perspective camera interface. */

#include "Engine/Math/Math.h"

namespace Engine {
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
         * @param fov Vertical field of view in degrees, in the range (0, 180).
         * @param aspectRatio Width-to-height ratio; must be positive.
         * @param nearPlane Distance to the near clipping plane.
         * @param farPlane Distance to the far clipping plane; must exceed @p nearPlane.
         * @throws std::invalid_argument If any parameter is invalid.
         */
        Camera(Degrees fov, float aspectRatio, float nearPlane, float farPlane);

        /** @brief Sets the camera world position. */
        void setPosition(const Vec3 &position);

        /**
         * @brief Sets yaw, pitch and roll in degrees; pitch is clamped to [-89, 89].
         * @param yaw Rotation around the world Y axis.
         * @param pitch Rotation around the camera's right axis.
         * @param roll Rotation around the camera's forward axis.
         */
        void setRotation(Degrees yaw, Degrees pitch, Degrees roll = Degrees{});

        /** @brief Translates the camera by a world-space offset. */
        void move(const Vec3 &offset);

        /** @brief Returns the view matrix derived from position and orientation. */
        [[nodiscard]] Mat4 viewMatrix() const;

        /** @brief Returns the Vulkan-compatible perspective projection matrix. */
        [[nodiscard]] Mat4 projectionMatrix() const;

        /** @brief Returns the Vulkan-compatible projection matrix without TAA jitter. */
        [[nodiscard]] Mat4 unjitteredProjectionMatrix() const;

        /** @brief Returns the camera world position. */
        [[nodiscard]] Vec3 position() const { return m_position; }

        /** @brief Returns the normalized forward direction. */
        [[nodiscard]] Vec3 forward() const;

        /** @brief Returns the normalized right direction. */
        [[nodiscard]] Vec3 right() const;

        /** @brief Returns the normalized up direction. */
        [[nodiscard]] Vec3 up() const;

        /** @brief Updates the aspect ratio used by the projection matrix. */
        void setAspectRatio(float aspectRatio);

        /** Applies a sub-pixel projection offset in Vulkan clip-space. */
        void setProjectionJitter(float x, float y) noexcept;

    private:
        Vec3 m_position{0.0F, 0.0F, 3.0F}; //NOLINT

        Degrees m_yaw{Degrees{-90.0F}}; //NOLINT
        Degrees m_pitch{};
        Degrees m_roll{};

        Degrees m_fov;
        float m_aspectRatio;
        float m_nearPlane;
        float m_farPlane;
        float m_jitterX = 0.0F;
        float m_jitterY = 0.0F;
    };
} // namespace Engine
