#pragma once

/**
 * @file CameraComponent.h
 * @brief Defines camera projection settings used by scene entities.
 */

#include <algorithm>
#include <cassert>

namespace Engine
{
    /**
     * @brief Projection mode used by a camera.
     */
    enum class CameraProjection : unsigned char
    {
        Perspective,
        Orthographic
    };

    /**
     * @brief Camera settings attached to an entity.
     *
     * The camera does not own a transform. The entity's TransformComponent is
     * the source of the camera position and rotation. Keeping the two
     * components separate allows the renderer to use the same transform for
     * culling, shadows and camera rendering.
     *
     * Angles are stored in degrees. The renderer is responsible for converting
     * them to radians when building the projection matrix and for applying the
     * Vulkan Y-axis correction required by its graphics API.
     */
    struct CameraComponent final
    {
        /// Projection used to construct the camera projection matrix.
        CameraProjection projection = CameraProjection::Perspective;

        /// Vertical field of view for perspective projection, in degrees.
        float fieldOfView = 60.0F;

        /// Visible height for orthographic projection, in world units.
        float orthographicSize = 10.0F;

        /// Distance to the near clipping plane. Must be greater than zero.
        float nearClip = 0.1F;

        /// Distance to the far clipping plane. Must be greater than @ref nearClip.
        float farClip = 1000.0F;

        /// Render-target width-to-height ratio.
        float aspectRatio = 16.0F / 9.0F;

        /// Whether this camera is selected as the primary scene camera.
        bool primary = true;

        /**
         * @brief Checks whether perspective projection is active.
         * @return True when @ref projection is Perspective.
         */
        [[nodiscard]] bool isPerspective() const noexcept
        {
            return projection == CameraProjection::Perspective;
        }

        /**
         * @brief Checks whether orthographic projection is active.
         * @return True when @ref projection is Orthographic.
         */
        [[nodiscard]] bool isOrthographic() const noexcept
        {
            return projection == CameraProjection::Orthographic;
        }

        /**
         * @brief Configures the camera for perspective projection.
         * @param fovDegrees Vertical field of view in degrees. It is clamped to
         *        the range [1, 179].
         * @param nearPlane Requested near clipping-plane distance.
         * @param farPlane Requested far clipping-plane distance.
         */
        void setPerspective(float fovDegrees, float nearPlane, float farPlane) noexcept
        {
            projection = CameraProjection::Perspective;
            fieldOfView = std::clamp(fovDegrees, 1.0F, 179.0F);
            nearClip = std::max(0.0001F, nearPlane);
            farClip = std::max(nearClip + 0.0001F, farPlane);
        }

        /**
         * @brief Configures the camera for orthographic projection.
         * @param height Visible orthographic height in world units.
         * @param nearPlane Requested near clipping-plane distance.
         * @param farPlane Requested far clipping-plane distance.
         */
        void setOrthographic(float height, float nearPlane, float farPlane) noexcept
        {
            projection = CameraProjection::Orthographic;
            orthographicSize = std::max(0.0001F, height);
            nearClip = std::max(0.0001F, nearPlane);
            farClip = std::max(nearClip + 0.0001F, farPlane);
        }

        /**
         * @brief Updates the aspect ratio from a render-target size.
         * @param width Render-target width in pixels or equivalent units.
         * @param height Render-target height in pixels or equivalent units.
         *
         * The ratio is not changed when either dimension is non-positive.
         */
        void setAspectRatio(float width, float height) noexcept
        {
            if (width > 0.0F && height > 0.0F)
                aspectRatio = width / height;
        }

        /**
         * @brief Sets the aspect ratio directly.
         * @param ratio Positive width-to-height ratio.
         *
         * The ratio is not changed when the supplied value is non-positive.
         */
        void setAspectRatio(float ratio) noexcept
        {
            if (ratio > 0.0F)
                aspectRatio = ratio;
        }

        /**
         * @brief Checks whether all camera parameters satisfy their invariants.
         * @return True when the clipping planes, aspect ratio and active
         *         projection settings are valid.
         */
        [[nodiscard]] bool isValid() const noexcept
        {
            if (!(aspectRatio > 0.0F) || !(nearClip > 0.0F) || !(farClip > nearClip))
                return false;

            if (isPerspective())
                return fieldOfView > 0.0F && fieldOfView < 180.0F;

            return orthographicSize > 0.0F;
        }

        /**
         * @brief Verifies camera invariants in debug builds.
         *
         * Setters already clamp values, but this is useful after deserializing
         * a scene file. In release builds, assertions may be disabled.
         */
        void validate() const noexcept
        {
            assert(isValid());
        }
    };
}